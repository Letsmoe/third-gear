#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "ShaderCore.h"

class FMapRuntimeModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		// Material Custom nodes include helpers from here: "/Plugin/MapRuntime/Private/....ush"
		const FString ShaderDir = FPaths::Combine(IPluginManager::Get().FindPlugin(TEXT("MapRuntime"))->GetBaseDir(), TEXT("Shaders"));
		AddShaderSourceDirectoryMapping(TEXT("/Plugin/MapRuntime"), ShaderDir);
	}
};

IMPLEMENT_MODULE(FMapRuntimeModule, MapRuntime)
