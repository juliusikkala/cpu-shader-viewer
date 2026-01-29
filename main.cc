#include <SDL3/SDL.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <vector>
#include <string>
#include <memory>

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
    uint8_t* pixelData;
    size_t pixelDataSize;
};

typedef void (*computeGroupEntryPoint)(
    int groupID[3],
    void* entryPointParams,
    RunnerGlobalParams* globalParams);

struct viewer_resources
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
    abort();
}

viewer_resources init()
{
    viewer_resources res;

    if (!SDL_Init(SDL_INIT_EVENTS|SDL_INIT_VIDEO))
        panic("Can't init, yikes. %s\n", SDL_GetError());

    res.window = SDL_CreateWindow("CPU shader viewer", 1280, 720, 0);

    if (!res.window)
        panic("Can't open window, yikes. %s\n", SDL_GetError());

    res.surf = SDL_GetWindowSurface(res.window);

    SlangGlobalSessionDesc desc = {};
    desc.enableGLSL = true;
    if (slang::createGlobalSession(&desc, res.globalSession.writeRef()) != SLANG_OK)
        panic("Failed to init Slang session\n");

    res.constants.reset(new ShaderViewerConstants);
    res.globalParams.constants = res.constants.get();
    return res;
}

void deinit(viewer_resources& res)
{
    SDL_DestroyWindow(res.window);
    SDL_Quit();
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

bool loadShaderFromSource(viewer_resources& res, const char* glslSource)
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

    source += glslSource;

    source += R"(
RWStructuredBuffer<uint8_t4> pixelData;
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
    pixelData[i] = uint8_t4(saturate(color) * 255);
}
)";

    slang::CompilerOptionEntry options[] = {
        {slang::CompilerOptionName::AllowGLSL, {{}, 1}},
        {slang::CompilerOptionName::EmitCPUMethod, {{}, SLANG_EMIT_CPU_VIA_LLVM}},
        {slang::CompilerOptionName::Optimization, {{}, SLANG_OPTIMIZATION_LEVEL_MAXIMAL}},
        {slang::CompilerOptionName::FloatingPointMode, {{}, SLANG_FLOATING_POINT_MODE_FAST}},
        {slang::CompilerOptionName::DenormalModeFp16, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::DenormalModeFp32, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::DenormalModeFp64, {{}, SLANG_FP_DENORM_MODE_ANY}},
        {slang::CompilerOptionName::LLVMCPU, {{}, 0, 0, "znver5"}},
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
    sessionDesc.allowGLSLSyntax = true;
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
    
    slang::IModule* glsl = res.session->loadModule("glsl");

    slang::IComponentType* components[] = {module, glsl, entryPoint};
    Slang::ComPtr<slang::IComponentType> program;
    SlangResult err = res.session->createCompositeComponentType(
        components,
        std::size(components),
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

bool loadShader(viewer_resources& res, const char* path)
{
    bool status = path ? 
        loadShaderFromSource(res, readTextFile(path).c_str()) : false;
    if (!status)
    {
        const char* fallbackSource = R"(
void mainImage(out vec4 fragColor, in vec2 fragCoord)
{
    fragColor = vec4(1.0,0.0,0.0,1.0);
}
)";
        loadShaderFromSource(res, fallbackSource);
    }
    return status;
}

void renderTile(viewer_resources& res, int xTile, int yTile)
{
    if (res.entryPointFunc)
    {
        int gid[3] = {xTile, yTile, 0};
        res.entryPointFunc(gid, nullptr, &res.globalParams);
    }
}

void renderFrame(viewer_resources& res, int width, int height)
{
    int xTiles = (width + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;
    int yTiles = (height + DISPATCH_TILE_SIZE - 1) / DISPATCH_TILE_SIZE;

    #pragma omp parallel for collapse(2) schedule(dynamic,1)
    for (int y = 0; y < yTiles; ++y)
    for (int x = 0; x < xTiles; ++x)
        renderTile(res, x, y);
}

int main()
{
    viewer_resources res = init();
    loadShader(res, nullptr);

    uint64_t prevTicks = SDL_GetTicksNS();
    uint64_t epochTicks = prevTicks;

    std::string activeShaderPath = "";
    SDL_Time shaderModifyTime;

    std::vector<uint8_t> framebuffer;

    auto& params = *res.constants;
    params.frame = 0;
    params.mouseX = 0;
    params.mouseY = 0;
    params.mouseClickX = 0;
    params.mouseClickY = 0;

    bool valid = false;

    uint8_t* pixelData;
    size_t pixelDataSize;
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

        framebuffer.resize(res.surf->w * res.surf->h * 4);

        res.globalParams.pixelData = framebuffer.data();
        res.globalParams.pixelDataSize = framebuffer.size() / 4;
        params.pitch = res.surf->w;
        params.resX = res.surf->w;
        params.resY = res.surf->h;
        params.resZ = 1;

        renderFrame(res, res.surf->w, res.surf->h);

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
    return 0;
}
