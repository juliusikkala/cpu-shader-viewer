#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>
#include <sstream>
#include <cmath>
#include <algorithm>
#include <filesystem>
#include <format>

#include "slang.h"
#include "slang-com-ptr.h"

static constexpr int DISPATCH_TILE_SIZE = 8;

struct ShaderViewerConstants
{
	float time;
    int32_t frame;
    uint32_t pitch;
    float mouseX, mouseY, mouseClickX, mouseClickY;
    float resX, resY, resZ;
};

struct RunnerGlobalParams
{
    ShaderViewerConstants* constants;
    uint32_t* pixelData;
    size_t pixelDataSize;
};

typedef void (*computeGroupEntryPoint)(
    int groupID[3],
    void* entryPointParams,
    RunnerGlobalParams* globalParams);

struct ViewerResources
{
    SDL_Window* window;
    SDL_Surface* surf;

    Slang::ComPtr<slang::IGlobalSession> globalSession;
    Slang::ComPtr<slang::ISession> session;
    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    Slang::ComPtr<ISlangSharedLibrary> sharedLibrary;

    computeGroupEntryPoint entryPointFunc = nullptr;

    std::unique_ptr<ShaderViewerConstants> constants;
    RunnerGlobalParams globalParams;
};

template<typename... Args>
void panic(Args&&... args)
{
    ((int (*)(const char *, ...))printf)(args...);
    exit(1);
}

ViewerResources init()
{
    ViewerResources res;

    if (!SDL_Init(SDL_INIT_EVENTS|SDL_INIT_VIDEO))
        panic("Can't init, yikes. %s\n", SDL_GetError());

    res.window = SDL_CreateWindow("CPU shader viewer", 1280, 720, 0);

    if (!res.window)
        panic("Can't open window, yikes. %s\n", SDL_GetError());

    res.surf = SDL_GetWindowSurface(res.window);
    if (!res.surf)
        panic("Can't get window surface, yikes. %s\n", SDL_GetError());
    SDL_ClearSurface(res.surf, 0, 0, 0, 0);

    SlangGlobalSessionDesc desc = {};
    desc.enableGLSL = true;
    if (slang::createGlobalSession(&desc, res.globalSession.writeRef()) != SLANG_OK)
        panic("Failed to init Slang session\n");

    res.constants.reset(new ShaderViewerConstants);
    res.globalParams.constants = res.constants.get();
    return res;
}

void deinit(ViewerResources& res)
{
    SDL_DestroyWindow(res.window);
    SDL_Quit();
}

void setResolution(ViewerResources& res, int w, int h)
{
    SDL_SetWindowSize(res.window, w, h);
    SDL_PumpEvents();
    res.surf = SDL_GetWindowSurface(res.window);
    SDL_ClearSurface(res.surf, 0, 0, 0, 0);
    SDL_UpdateWindowSurface(res.window);
    SDL_UpdateWindowSurface(res.window);
}

std::string readTextFile(const char* path)
{
    FILE* f = fopen(path, "rb");

    if(!f) panic("Unable to open %s\n", path);

    fseek(f, 0, SEEK_END);
    size_t sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* data = new char[sz];
    if(fread(data, 1, sz, f) != sz)
    {
        fclose(f);
        delete [] data;
        panic("Unable to read %s\n", path);
    }
    fclose(f);
    std::string ret(data, sz);

    delete [] data;
    return ret;
}

bool isPathToGLSL(const char* path)
{
    return std::filesystem::path(path).extension().string() == ".glsl";
}

bool loadShaderFromSource(ViewerResources& res, const char* shaderSource, bool allowGLSL)
{
    res.entryPointFunc = nullptr;

    std::string source;
    source = R"(
struct ShaderViewerConstants
{
    float time;
    int frame;
    uint pitch;
    float4 mouse;
    float3 res;
};

ConstantBuffer<ShaderViewerConstants, CDataLayout> shaderViewerConstants;

#define iResolution shaderViewerConstants.res
#define iFrame shaderViewerConstants.frame
#define iMouse shaderViewerConstants.mouse
#define iTime shaderViewerConstants.time
)";

    source += shaderSource;

    source += R"(
RWStructuredBuffer<uint32_t> pixelData;
)";

    std::string numStr = std::to_string(DISPATCH_TILE_SIZE);
    source += "[numthreads(" + numStr + ", " + numStr + ", 1)]";

    source += R"(
void renderRunner(
    uint3 dispatchThreadID : SV_DispatchThreadID,
    uint3 groupThreadID : SV_GroupThreadID)
{
    float4 color = float4(1);
    float2 p = float2(dispatchThreadID.xy) + float2(0.5);
    p.y = shaderViewerConstants.res.y - p.y;
    mainImage(color, p);

    uint i = dispatchThreadID.x + dispatchThreadID.y * shaderViewerConstants.pitch;
    uint4 ucolor = uint4(saturate(color) * 255);
    pixelData[i] = (ucolor.a << 24) | (ucolor.b << 16) | (ucolor.g << 8) | ucolor.r;
}
)";

    //printf("%s\n", source.c_str());

    slang::CompilerOptionEntry options[] = {
        {slang::CompilerOptionName::AllowGLSL, {{}, allowGLSL ? 1 : 0}},
        {slang::CompilerOptionName::EmitCPUMethod, {{}, SLANG_EMIT_CPU_VIA_LLVM}},
        {slang::CompilerOptionName::Optimization, {{}, SLANG_OPTIMIZATION_LEVEL_MAXIMAL}},
        {slang::CompilerOptionName::FloatingPointMode, {{}, SLANG_FLOATING_POINT_MODE_FAST}},
        {slang::CompilerOptionName::DenormalModeFp16, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::DenormalModeFp32, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::DenormalModeFp64, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::DownstreamArgs, {{}, 0, 0, "llvm", "-vector-library=AMDLIBM"}}
        //{slang::CompilerOptionName::DumpIr, {{}, 1}}
    };

    slang::TargetDesc target = {};
    target.format = SLANG_SHADER_HOST_CALLABLE;
    target.compilerOptionEntries = options;
    target.compilerOptionEntryCount = std::size(options);

    slang::SessionDesc sessionDesc;
    sessionDesc.targets = &target;
    sessionDesc.targetCount = 1;
    sessionDesc.allowGLSLSyntax = allowGLSL;
    sessionDesc.compilerOptionEntries = options;
    sessionDesc.compilerOptionEntryCount = std::size(options);

    if (res.globalSession->createSession(sessionDesc, res.session.writeRef()))
    {
        fprintf(stderr, "Failed to open session!\n");
        return false;
    }

    Slang::ComPtr<slang::IBlob> diagnosticBlob;
    slang::IModule* module = res.session->loadModuleFromSourceString("runner", "shader.slang", source.c_str(), diagnosticBlob.writeRef());
    if (diagnosticBlob)
        fprintf(stderr, "%s\n", (const char*)diagnosticBlob->getBufferPointer());
    if (!module)
    {
        return false;
    }

    Slang::ComPtr<slang::IEntryPoint> entryPoint;
    module->findEntryPointByName("renderRunner", entryPoint.writeRef());
    
    slang::IModule* glsl;
    if (allowGLSL)
        glsl = res.session->loadModule("glsl");

    slang::IComponentType* components[] = {module, entryPoint, glsl};
    Slang::ComPtr<slang::IComponentType> program;
    SlangResult err = res.session->createCompositeComponentType(
        components,
        allowGLSL ? 3 : 2,
        program.writeRef(),
        diagnosticBlob.writeRef());
    if (diagnosticBlob)
        fprintf(stderr, "%s\n", (const char*)diagnosticBlob->getBufferPointer());
    if (err != SLANG_OK)
    {
        return false;
    }

    if (program->getEntryPointHostCallable(
            0,
            0,
            res.sharedLibrary.writeRef(),
            diagnosticBlob.writeRef()) != SLANG_OK)
    {
        fprintf(stderr, "%s\n", (const char*)diagnosticBlob->getBufferPointer());
        return false;
    }

    res.entryPointFunc =
        (computeGroupEntryPoint)res.sharedLibrary->findFuncByName("renderRunner_Group");
    if (!res.entryPointFunc)
    {
        fprintf(stderr, "Failed to find entry point!\n");
        return false;
    }
    return true;
}

bool loadShader(ViewerResources& res, const char* path)
{
    bool status = path ? 
        loadShaderFromSource(res, readTextFile(path).c_str(), isPathToGLSL(path)) : false;
    if (!status)
    {
        const char* fallbackSource = R"(
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    fragColor = vec4(1.0,0.0,0.0,1.0);
}
)";
        loadShaderFromSource(res, fallbackSource, true);
    }
    return status;
}

void renderTile(ViewerResources& res, int xTile, int yTile)
{
    if (res.entryPointFunc)
    {
        int gid[3] = {xTile, yTile, 0};
        res.entryPointFunc(gid, nullptr, &res.globalParams);
    }
}

void renderFrameMultithread(ViewerResources& res, int width, int height)
{
    int xTiles = (width + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;
    int yTiles = (height + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;

    #pragma omp parallel for collapse(2) schedule(dynamic,1)
    for (int y = 0; y < yTiles; ++y)
    for (int x = 0; x < xTiles; ++x)
        renderTile(res, x, y);
}

void renderFrameSinglethread(ViewerResources& res, int width, int height)
{
    int xTiles = (width + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;
    int yTiles = (height + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;

    for (int y = 0; y < yTiles; ++y)
    for (int x = 0; x < xTiles; ++x)
        renderTile(res, x, y);
}

void printUsage(FILE* out, char* programName)
{
    fprintf(out,
        "Usage: %s [benchmark-command-list-file]\n"
        "Check the README for how the benchmark command list works.\n",
        programName);
}

void interactiveMain()
{
    ViewerResources res = init();
    loadShader(res, nullptr);

    uint64_t prevTicks = SDL_GetTicksNS();
    uint64_t epochTicks = prevTicks;

    std::string activeShaderPath = "";
    SDL_Time shaderModifyTime;

    std::vector<uint32_t> framebuffer;

    auto& params = *res.constants;
    params.frame = 0;
    params.mouseX = 0;
    params.mouseY = 0;
    params.mouseClickX = 0;
    params.mouseClickY = 0;

    bool valid = false;

    for(;;)
    {
        uint64_t curTicks = SDL_GetTicksNS();

        float deltaTime = (curTicks - prevTicks) * 1e-9f;
        float totalTime = (curTicks - epochTicks) * 1e-9f;

        params.time = totalTime;
        if (valid)
            printf("%f\n", deltaTime);

        prevTicks = curTicks;

        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_Q)
                    goto end;
                if (event.key.key == SDLK_R)
                    epochTicks = curTicks;
                break;
            case SDL_EVENT_DROP_FILE:
                {
                    const char* path = event.drop.data;
                    SDL_PathInfo info;
                    if (SDL_GetPathInfo(event.drop.data, &info))
                    {
                        if (loadShader(res, event.drop.data))
                        {
                            activeShaderPath = event.drop.data;
                            shaderModifyTime = info.modify_time;
                            valid = true;
                        }
                        else valid = false;
                    }
                }
                break;
            case SDL_EVENT_QUIT:
                goto end;
            }
        }

        SDL_PathInfo info;
        if (SDL_GetPathInfo(activeShaderPath.c_str(), &info))
        {
            if (shaderModifyTime < info.modify_time)
            {
                shaderModifyTime = info.modify_time;
                valid = loadShader(res, activeShaderPath.c_str());
            }
        }

        framebuffer.resize(res.surf->w * res.surf->h);

        res.globalParams.pixelData = framebuffer.data();
        res.globalParams.pixelDataSize = framebuffer.size();
        params.pitch = res.surf->w;
        params.resX = res.surf->w;
        params.resY = res.surf->h;
        params.resZ = 1;

        renderFrameMultithread(res, res.surf->w, res.surf->h);

        SDL_LockSurface(res.surf);
        SDL_ConvertPixels(
            res.surf->w, res.surf->h, SDL_PIXELFORMAT_ABGR8888, framebuffer.data(),
            res.surf->w * 4, res.surf->format, res.surf->pixels, res.surf->pitch);
        SDL_UnlockSurface(res.surf);

        SDL_UpdateWindowSurface(res.window);

        params.frame++;
    }

end:

    deinit(res);
}

void skipWhitespace(const char*& str)
{
    while(*str != 0 && strchr(" \t\n", *str)) str++;
}

std::string readUntilWhitespace(const char*& str)
{
    std::string token;
    while (*str != ' ' && *str != '\t' && *str)
    {
        token += *str;
        str++;
    }
    return token;
}

bool readDouble(const std::string& str, double& d)
{
    char* out = nullptr;
    d = strtod(str.c_str(), &out);
    return *out == 0;
}

std::vector<std::string> splitByWhitespace(const char* str)
{
    std::vector<std::string> args;
    skipWhitespace(str);
    while (*str)
    {
        args.push_back(readUntilWhitespace(str));
        skipWhitespace(str);
    }
    return args;
}

float sum(const std::vector<float>& vals)
{
    float s = 0;
    for (float v: vals)
        s += v;
    return s;
}

float mean(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    float s = 0;
    for (float v: vals)
        s += v;
    return s / vals.size();
}

float min(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    float s = vals[0];
    for (float v: vals)
        s = v < s ? v : s;
    return s;
}

float max(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    float s = vals[0];
    for (float v: vals)
        s = v > s ? v : s;
    return s;
}

float median(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    // The brainlet algorithm.
    std::vector<float> v = vals;
    std::sort(v.begin(), v.end());
    return v[v.size()/2];
}

float geomean(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    float s = 1.0;
    for (float v: vals)
        s *= v;
    return pow(s, 1.0 / vals.size());
}

float harmonicMean(const std::vector<float>& vals)
{
    if (vals.size() == 0)
        return 0.0f;

    float s = 0;
    for (float v: vals)
        s += 1.0 / v;
    return vals.size() / s;
}

float variance(const std::vector<float>& vals)
{
    float m = mean(vals);
    float s = 0;
    for (float v: vals)
        s += (m-v) * (m-v);
    return s / vals.size();
}

float collect(const std::vector<float>& vals, const char* cumulative)
{
    if (!cumulative)
        return vals.size() == 0 ? 0 : vals.back();
    else if (strcmp(cumulative, "sum") == 0)
        return sum(vals);
    else if (strcmp(cumulative, "mean") == 0)
        return mean(vals);
    else if (strcmp(cumulative, "min") == 0)
        return min(vals);
    else if (strcmp(cumulative, "max") == 0)
        return max(vals);
    else if (strcmp(cumulative, "median") == 0)
        return median(vals);
    else if (strcmp(cumulative, "geomean") == 0)
        return geomean(vals);
    else if (strcmp(cumulative, "harmonic-mean") == 0)
        return harmonicMean(vals);
    else if (strcmp(cumulative, "variance") == 0)
        return variance(vals);
    else if (strcmp(cumulative, "stddev") == 0)
        return sqrt(variance(vals));
    else
        panic("Unknown cumulation prefix %s\n", cumulative);
    return 0;
}

struct RunStats
{
    float buildTime;
    std::vector<float> frames;
};

struct Stats
{
    std::vector<RunStats> runs;

    void clear()
    {
        runs.clear();
    }

    float getStat(const std::string& spec)
    {
        float v = 0;
        std::vector<std::string> specifiers = splitByWhitespace(spec.c_str());

        if (specifiers.size() == 0)
            panic("No variable name given!");

        std::string var = specifiers.back();
        specifiers.pop_back();

        std::vector<float> stats;
        if (var == "build-time")
        {
            for (RunStats r: runs)
                stats.push_back(r.buildTime);
        }
        else if (var == "frame-time")
        {
            const char* cumulation = nullptr;
            if (specifiers.size() >= 1)
                cumulation = specifiers.back().c_str();

            for (RunStats r: runs)
                stats.push_back(collect(r.frames, cumulation));

            if (cumulation)
                specifiers.pop_back();
        }
        else
            panic("Unknown variable %s\n", var.c_str());

        if (specifiers.size() > 1)
            panic("Too many cumulation prefixes in \"%s\"!\n", spec.c_str());

        return collect(stats, specifiers.size() == 0 ? nullptr : specifiers.back().c_str());
    }
};

void benchmarkRenderMain(ViewerResources& res, Stats& stats, const char* shaderPath, int frameCount, double forcedDeltaTime, bool multithreaded)
{
    RunStats run;

    std::string shaderSource = readTextFile(shaderPath);

    uint64_t buildStartTicks = SDL_GetTicksNS();
    if (!loadShaderFromSource(res, shaderSource.c_str(), isPathToGLSL(shaderPath)))
        panic("Failed to load shader %s\n", shaderPath);
    uint64_t buildFinishTicks = SDL_GetTicksNS();
    run.buildTime = (buildFinishTicks-buildStartTicks) * 1e-9;

    auto& params = *res.constants;
    params.frame = 0;
    params.mouseX = 0;
    params.mouseY = 0;
    params.mouseClickX = 0;
    params.mouseClickY = 0;

    std::vector<uint32_t> framebuffer;
    framebuffer.resize(res.surf->w * res.surf->h);

    res.globalParams.pixelData = framebuffer.data();
    res.globalParams.pixelDataSize = framebuffer.size();
    params.pitch = res.surf->w;
    params.resX = res.surf->w;
    params.resY = res.surf->h;
    params.resZ = 1;

    uint64_t startTicks = SDL_GetTicksNS();
    uint64_t cumulatedTicks = 0;

    for (; params.frame < frameCount; ++params.frame)
    {
        uint64_t curTicks = SDL_GetTicksNS();
        uint64_t deltaTicks = curTicks - startTicks;
        if (forcedDeltaTime > 0)
            deltaTicks = round(forcedDeltaTime * 1e9);

        if (params.frame != 0)
            cumulatedTicks += deltaTicks;

        startTicks = curTicks;
        params.time = cumulatedTicks * 1e-9;

        // Avoid getting the "program is unresponsive" message.
        SDL_Event event;
        while (SDL_PollEvent(&event))
        {
            switch (event.type)
            {
            case SDL_EVENT_QUIT:
                panic("User interrupted benchmark.");
                break;
            }
        }

        uint64_t renderStartTicks = SDL_GetTicksNS();
        if (multithreaded)
            renderFrameMultithread(res, res.surf->w, res.surf->h);
        else
            renderFrameSinglethread(res, res.surf->w, res.surf->h);
        uint64_t renderFinishTicks = SDL_GetTicksNS();

        float frameTime = (renderFinishTicks - renderStartTicks) * 1e-9;
        run.frames.push_back(frameTime);

        SDL_LockSurface(res.surf);
        SDL_ConvertPixels(
            res.surf->w, res.surf->h, SDL_PIXELFORMAT_ABGR8888, framebuffer.data(),
            res.surf->w * 4, res.surf->format, res.surf->pixels, res.surf->pitch);
        SDL_UnlockSurface(res.surf);

        SDL_UpdateWindowSurface(res.window);
    }

    stats.runs.emplace_back(run);
}

void benchmarkMain(const char* commandListPath)
{
    Stats stats;
    ViewerResources res = init();
    loadShader(res, nullptr);

    std::string commandList = readTextFile(commandListPath);
    std::istringstream input(commandList);

    double forcedDeltaTime = -1.0;
    bool multithreaded = true;

    for (std::string command; std::getline(input, command);)
    {
        // Strip whitespace from command
        const char* cmd = command.c_str();
        skipWhitespace(cmd);

        // Skip comments
        if (*cmd == '#' || !*cmd)
            continue;

        // Read operation
        std::string op = readUntilWhitespace(cmd);

        // Skip first space after command, this is important for 'print'.
        if (*cmd) cmd++;

        std::vector<std::string> args = splitByWhitespace(cmd);

        auto checkArgCount = [&](int count)
        {
            if (args.size() != count)
            {
                panic(
                    "Incorrect number of arguments for %s: expected %d, got %d\n",
                    op.c_str(), count, (int)args.size());
            }
        };

        auto argDouble = [&](int index)
        {
            double val;
            if (!::readDouble(args[index], val))
                panic("%s: expected number in argument %d\n", op.c_str(), index+1);
            return val;
        };

        if (op == "framerate")
        {
            checkArgCount(1);
            forcedDeltaTime = 1.0 / argDouble(0);
        }
        else if (op == "clear")
        {
            checkArgCount(0);
            stats.clear();
        }
        else if (op == "resolution")
        {
            checkArgCount(2);
            int w = int(argDouble(0));
            int h = int(argDouble(1));
            w = w < 1 ? 1 : w;
            h = h < 1 ? 1 : h;
            w = w > 8192 ? 8192 : w;
            h = h > 8192 ? 8192 : h;

            w = (w+DISPATCH_TILE_SIZE-1)/DISPATCH_TILE_SIZE*DISPATCH_TILE_SIZE;
            h = (h+DISPATCH_TILE_SIZE-1)/DISPATCH_TILE_SIZE*DISPATCH_TILE_SIZE;

            setResolution(res, w, h);
        }
        else if (op == "multithreading")
        {
            checkArgCount(1);
            if (args[0] == "on" || args[0] == "true")
                multithreaded = true;
            else
                multithreaded = false;
        }
        else if (op == "run")
        {
            checkArgCount(2);
            int numFrames = int(argDouble(1));
            benchmarkRenderMain(res, stats, args[0].c_str(), numFrames, forcedDeltaTime, multithreaded);
        }
        else if (op == "print")
        {
            std::string output;

            while (*cmd != 0)
            {
                if (cmd[0] == '$' && cmd[1] == '{')
                {
                    std::string spec;
                    cmd += 2;
                    while (*cmd && *cmd != '}')
                    {
                        spec += *cmd;
                        cmd++;
                    }

                    if (*cmd == '}')
                        cmd++;

                    output += std::to_string(stats.getStat(spec));
                }
                else
                {
                    output.push_back(*cmd);
                    cmd++;
                }
            }

            printf("%s\n", output.c_str());
        }
        else
        {
            panic("Unrecognized command %s\n", op.c_str());
        }
    }

    deinit(res);
}

int main(int argc, char** argv)
{
    if (argc <= 1)
    {
        // No args, interactive mode.
        interactiveMain();
        return 0;
    }
    else if (argc == 2)
    {
        benchmarkMain(argv[1]);
        return 0;
    }
    else
    {
        printUsage(stdout, argv[0]);
        return 1;
    }

    return 0;
}
