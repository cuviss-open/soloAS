#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"

#include "Misc/Paths.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FPaths(FAngelscriptBinds::EOrder::Late, []
{
	FAngelscriptBinds::FNamespace ns("FPaths");

	// global paths
	FAngelscriptBinds::BindGlobalFunction("FString RootDir() no_discard", FUNC_NOJIT(FPaths::RootDir));
	FAngelscriptBinds::BindGlobalFunction("FString LaunchDir() no_discard", FUNC_NOJIT(FPaths::LaunchDir));

	FAngelscriptBinds::BindGlobalFunction("FString CombinePaths(const FString& FirstPath, const FString& SecondPath) no_discard", [](const FString& FirstPath, const FString& SecondPath) { return FPaths::Combine(FirstPath, SecondPath); });

	FAngelscriptBinds::BindGlobalFunction("FString EngineDir() no_discard", FUNC_NOJIT(FPaths::EngineDir));
	FAngelscriptBinds::BindGlobalFunction("FString EngineContentDir() no_discard", FUNC_NOJIT(FPaths::EngineContentDir));
	FAngelscriptBinds::BindGlobalFunction("FString EngineConfigDir() no_discard", FUNC_NOJIT(FPaths::EngineConfigDir));
	FAngelscriptBinds::BindGlobalFunction("FString EngineEditorSettingsDir() no_discard", FUNC_NOJIT(FPaths::EngineEditorSettingsDir));
	FAngelscriptBinds::BindGlobalFunction("FString EngineIntermediateDir() no_discard", FUNC_NOJIT(FPaths::EngineIntermediateDir));
	FAngelscriptBinds::BindGlobalFunction("FString EngineSavedDir() no_discard", FUNC_NOJIT(FPaths::EngineSavedDir));

	FAngelscriptBinds::BindGlobalFunction("FString ProjectDir() no_discard", FUNC_NOJIT(FPaths::ProjectDir));
	FAngelscriptBinds::BindGlobalFunction("FString ProjectUserDir() no_discard", FUNC_NOJIT(FPaths::ProjectUserDir));
	FAngelscriptBinds::BindGlobalFunction("FString ProjectContentDir() no_discard", FUNC_NOJIT(FPaths::ProjectContentDir));
	FAngelscriptBinds::BindGlobalFunction("FString ProjectConfigDir() no_discard", FUNC_NOJIT(FPaths::ProjectConfigDir));
	FAngelscriptBinds::BindGlobalFunction("const FString& ProjectSavedDir() no_discard", FUNC_NOJIT(FPaths::ProjectSavedDir));
	FAngelscriptBinds::BindGlobalFunction("FString ProjectIntermediateDir() no_discard", FUNC_NOJIT(FPaths::ProjectIntermediateDir));

	FAngelscriptBinds::BindGlobalFunction("FString ScreenShotDir() no_discard", FUNC_NOJIT(FPaths::ScreenShotDir));
	FAngelscriptBinds::BindGlobalFunction("FString VideoCaptureDir() no_discard", FUNC_NOJIT(FPaths::VideoCaptureDir));

	FAngelscriptBinds::BindGlobalFunction("const FString& GetRelativePathToRoot() no_discard", FUNC_NOJIT(FPaths::GetRelativePathToRoot));

	// filename & path functions
	FAngelscriptBinds::BindGlobalFunction("FString GetExtension(const FString& InPath, bool bIncludeDot = false) no_discard", FUNCPR_NOJIT(FString, FPaths::GetExtension, (const FString&, bool)));
	FAngelscriptBinds::BindGlobalFunction("FString GetCleanFilename(const FString& InPath) no_discard",
		[](const FString& InPath) -> FString { return FPaths::GetCleanFilename(InPath); });
	FAngelscriptBinds::BindGlobalFunction("FString GetBaseFilename(const FString& InPath, bool bRemovePath = true) no_discard",
		[](const FString& InPath, bool bRemovePath) -> FString { return FPaths::GetBaseFilename(InPath, bRemovePath); });
	FAngelscriptBinds::BindGlobalFunction("FString GetPath(const FString& InPath) no_discard",
		[](const FString& InPath) -> FString { return FPaths::GetPath(InPath); });
	FAngelscriptBinds::BindGlobalFunction("FString GetPathLeaf(const FString& InPath) no_discard",
		[](const FString& InPath) -> FString { return FPaths::GetPathLeaf(InPath); });
	FAngelscriptBinds::BindGlobalFunction("FString ChangeExtension(const FString& InPath, const FString& InNewExtension) no_discard", FUNC_NOJIT(FPaths::ChangeExtension));
	FAngelscriptBinds::BindGlobalFunction("FString SetExtension(const FString& InPath, const FString& InNewExtension) no_discard", FUNC_NOJIT(FPaths::SetExtension));
	FAngelscriptBinds::BindGlobalFunction("void Split(const FString& InPath, FString& PathPart, FString& FilenamePart, FString& ExtensionPart) no_discard", FUNC_NOJIT(FPaths::Split));

	FAngelscriptBinds::BindGlobalFunction("bool FileExists(const FString& InPath) no_discard", FUNC_NOJIT(FPaths::FileExists));
	FAngelscriptBinds::BindGlobalFunction("bool DirectoryExists(const FString& InPath) no_discard", FUNC_NOJIT(FPaths::DirectoryExists));
	FAngelscriptBinds::BindGlobalFunction("bool IsDrive(const FString& InPath) no_discard", FUNC_NOJIT(FPaths::IsDrive));
	FAngelscriptBinds::BindGlobalFunction("bool IsRelative(const FString& InPath) no_discard", FUNC_NOJIT(FPaths::IsRelative));
	FAngelscriptBinds::BindGlobalFunction("bool IsRestrictedPath(const FString& InPath) no_discard", FUNC_NOJIT(FPaths::IsRestrictedPath));
	FAngelscriptBinds::BindGlobalFunction("bool IsSamePath(const FString& PathA, const FString& PathB) no_discard", FUNC_NOJIT(FPaths::IsSamePath));
	FAngelscriptBinds::BindGlobalFunction("bool IsUnderDirectory(const FString& InPath, const FString& InDirectory) no_discard", FUNC_NOJIT(FPaths::IsUnderDirectory));

	FAngelscriptBinds::BindGlobalFunction("void NormalizeFilename(FString& InPath)", FUNC_NOJIT(FPaths::NormalizeFilename));
	FAngelscriptBinds::BindGlobalFunction("void NormalizeDirectoryName(FString& InPath)", FUNC_NOJIT(FPaths::NormalizeDirectoryName));
	FAngelscriptBinds::BindGlobalFunction("bool CollapseRelativeDirectories(FString& InPath)", FUNC_NOJIT(FPaths::CollapseRelativeDirectories));
	FAngelscriptBinds::BindGlobalFunction("void RemoveDuplicateSlashes(FString& InPath)", FUNCPR(void, FPaths::RemoveDuplicateSlashes, (FString&)));
	FAngelscriptBinds::BindGlobalFunction("void MakeStandardFilename(FString& InPath)", FUNC_NOJIT(FPaths::MakeStandardFilename));
	FAngelscriptBinds::BindGlobalFunction("void MakePlatformFilename(FString& InPath)", FUNC_NOJIT(FPaths::MakePlatformFilename));
	FAngelscriptBinds::BindGlobalFunction("FString ConvertRelativePathToFull(const FString& InPath) no_discard",
		[](const FString& InPath) -> FString { return FPaths::ConvertRelativePathToFull(InPath); });
	FAngelscriptBinds::BindGlobalFunction("FString ConvertRelativePathToFull(const FString& BasePath, const FString& InPath) no_discard",
		[](const FString& BasePath, const FString& InPath) -> FString {return FPaths::ConvertRelativePathToFull(BasePath, InPath);});
});
