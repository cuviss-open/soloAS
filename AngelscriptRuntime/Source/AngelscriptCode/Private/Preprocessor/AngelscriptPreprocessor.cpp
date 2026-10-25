#include "Preprocessor/AngelscriptPreprocessor.h"

                                                                                                          
#include "Extensions/AngelscriptExtensionPipelineInternal.h"
                                                                                                        
#include "GameFramework/Actor.h"
#include "Misc/ScopedSlowTask.h"

#include "Helper_CommentFormat.h"

#include "AngelscriptSettings.h"
#include "AngelscriptType.h"

#include "Misc/PackageName.h"
#include "Modules/ModuleManager.h"
                                                                                                                      
#include "UObject/TextProperty.h"
                                                                                                                    

#include "StartAngelscriptHeaders.h"
#include "as_scriptengine.h"
#include "EndAngelscriptHeaders.h"

#include "Async/Async.h"
#include "Async/AsyncFileHandle.h"
#include "Internationalization/Regex.h"
#include "HAL/PlatformFileManager.h"
                                                                                                                          
#include "HAL/FileManager.h"
                                                                                                                        
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#include "AngelscriptCodeModule.h"

#include "Subsystems/Subsystem.h"
#include "Subsystems/EngineSubsystem.h"
#include "Subsystems/LocalPlayerSubsystem.h"
#include "Subsystems/WorldSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#if WITH_EDITOR
#include "EditorSubsystem.h"
#endif

static int32 GAngelscriptNewIterators = 1;
static FAutoConsoleVariableRef CVar_AngelscriptNewIterators(TEXT("angelscript.UseNewIterators"), GAngelscriptNewIterators, TEXT(""));

FOnAngelscriptPreprocessHook FAngelscriptPreprocessor::OnProcessChunks;
FOnAngelscriptPreprocessHook FAngelscriptPreprocessor::OnPostProcessCode;

FOnAngelscriptModifyClassDefaults FAngelscriptPreprocessor::OnModifyClassDefaults;
FOnAngelscriptModifyFunctionDefaults FAngelscriptPreprocessor::OnModifyFunctionDefaults;
FOnAngelscriptModifyPropertyDefaults FAngelscriptPreprocessor::OnModifyPropertyDefaults;
FAngelscriptShouldRemoveFunctionOutsideEditor FAngelscriptPreprocessor::ShouldRemoveFunctionOutsideEditor;

FAngelscriptPreprocessor::FAngelscriptPreprocessor(const TMap<FName, FString>& InBlueprintBindings)
	: ProjectBlueprintBindings(InBlueprintBindings)
{
	PreprocessorFlags.Add(TEXT("EDITOR"), FAngelscriptManager::bUseEditorScripts);
	PreprocessorFlags.Add(TEXT("EDITORONLY_DATA"), WITH_EDITORONLY_DATA && ((!IsRunningGame() && !IsRunningDedicatedServer()) || FAngelscriptManager::bUseEditorScripts));
	PreprocessorFlags.Add(TEXT("COOK_COMMANDLET"), IsRunningCookCommandlet());
	PreprocessorFlags.Add(TEXT("RELEASE"), UE_BUILD_SHIPPING || UE_BUILD_TEST);
	PreprocessorFlags.Add(TEXT("TEST"), !UE_BUILD_SHIPPING);
	PreprocessorFlags.Add(TEXT("WITH_SERVER_CODE"), WITH_SERVER_CODE);

	auto AngelscriptSettings = UAngelscriptSettings::StaticClass()->GetDefaultObject<UAngelscriptSettings>();
	for (auto& Flag : AngelscriptSettings->PreprocessorFlags)
	{
		PreprocessorFlags.Add(Flag, true);
		PreprocessorFlagsFromIni.Add(Flag);
	}

	bDefaultFunctionBlueprintCallable = AngelscriptSettings->bDefaultFunctionBlueprintCallable;
	DefaultPropertyEditSpecifier = AngelscriptSettings->DefaultPropertyEditSpecifier;
	DefaultPropertyEditSpecifierForStructs = AngelscriptSettings->DefaultPropertyEditSpecifierForStructs;
	DefaultPropertyBlueprintSpecifier = AngelscriptSettings->DefaultPropertyBlueprintSpecifier;

#if WITH_EDITOR
	if (FAngelscriptManager::bSimulateCooked)
	{
		PreprocessorFlags.Add(TEXT("EDITOR"), false);
		PreprocessorFlags.Add(TEXT("EDITORONLY_DATA"), false);
		PreprocessorFlags.Add(TEXT("RELEASE"), true);
		PreprocessorFlags.Add(TEXT("TEST"), false);
	}

	if (FAngelscriptManager::bForcePreprocessEditorCode)
	{
		PreprocessorFlags.Add(TEXT("EDITOR"), true);
		PreprocessorFlags.Add(TEXT("EDITORONLY_DATA"), true);
	}
#endif
}

TArray<TSharedRef<FAngelscriptModuleDesc>> FAngelscriptPreprocessor::GetModulesToCompile()
{
	ensureMsgf(bIsPreprocessed, TEXT("Must preprocess before retrieving modules."));

	TArray<TSharedRef<FAngelscriptModuleDesc>> OutArray;
	for (auto& File : Files)
		OutArray.AddUnique(File.Module.ToSharedRef());

	return OutArray;
}

FString FAngelscriptPreprocessor::FilenameToModuleName(const FString& Filename)
{
	return Filename.Replace(TEXT(".as"), TEXT("")).Replace(TEXT("/"), TEXT("."));
}

void FAngelscriptPreprocessor::AddFile(const FString& RelativeFilename, const FString& AbsoluteFilename, bool bLoadAsynchronous, bool bTreatAsDeleted)
{
	if (!ensureMsgf(!bIsPreprocessed, TEXT("Cannot add files after preprocessing is done.")))
		return;

	FFile& File = Files.AddDefaulted_GetRef();

	TSharedRef<FAngelscriptModuleDesc> Module = MakeShared<FAngelscriptModuleDesc>();
	Module->ModuleName = FilenameToModuleName(RelativeFilename);
                                                                                              
	if (Files.Num() == 1 && !bHasScriptUnitCatalog)
		ScriptUnits.Discover();
	Module->ScriptUnit = ScriptUnits.ResolveOwner(AbsoluteFilename);
	Module->ModuleName = ScriptUnits.ResolveModuleName(AbsoluteFilename, RelativeFilename);
	Module->bTreatAsDeleted = bTreatAsDeleted;
                                                                                            

	File.Module = Module;
	File.AbsoluteFilename = AbsoluteFilename;
	File.RelativeFilename = RelativeFilename;
	File.bTreatAsDeleted = bTreatAsDeleted;

#if WITH_EDITOR
	File.bEditorOnlyModule = Module->ModuleName.StartsWith(TEXT("Editor.")) || Module->ModuleName.Contains(TEXT(".Editor."));
#endif

	if (bTreatAsDeleted)
	{
		File.RawCode = TEXT("");
	}
	else if (bLoadAsynchronous)
	{
		File.bLoadAsynchronous = true;
		bLoadingAnyFilesAsynchronous = true;
	}
	else
	{
		bool bLoaded = false;

		int32 Tries = 0;
		for (; Tries < 6; ++Tries)
		{
                                                                                                                  
			bool bReadWithinBudget = false;
			if (!OnProcessChunks.IsBound())
				bReadWithinBudget = FFileHelper::LoadFileToString(File.RawCode, *AbsoluteFilename);
			else
			{
				TUniquePtr<FArchive> Source(IFileManager::Get().CreateFileReader(*AbsoluteFilename, FILEREAD_Silent));
				if (Source)
				{
					const int64 Size = Source->TotalSize();
					if (Size >= 0 && Size <= 16 * 1024 * 1024)
					{
						TArray<uint8> Bytes; Bytes.SetNumUninitialized(static_cast<int32>(Size));
						if (Size != 0) Source->Serialize(Bytes.GetData(), Size);
						if (!Source->IsError())
						{ FFileHelper::BufferToString(File.RawCode, Bytes.GetData(), Bytes.Num()); bReadWithinBudget = true; }
					}
					else { bHasError = true; File.Module->bCompileError = true; }
				}
			}

			if (bReadWithinBudget)
                                                                                                                
			{
				bLoaded = true;
				break;
			}

			if (Tries >= 4)
				FPlatformProcess::Sleep(0.2f);
			else if (Tries >= 3)
				FPlatformProcess::Sleep(0.1f);
			else if (Tries >= 2)
				FPlatformProcess::Sleep(0.01f);
		}

		if (!bLoaded)
			UE_LOG(Angelscript, Warning, TEXT("Unable to open script file %s after several retries. Treating file as deleted."), *AbsoluteFilename);
		}

}

void FAngelscriptPreprocessor::PerformAsynchronousLoads()
{
	FAngelscriptScopeTimer ReadTimer(TEXT("asynchronous load script files from disk"));

	// Kickoff asynchronous loads for all files
	for (FFile& File : Files)
	{
		if (!File.bLoadAsynchronous)
			continue;

		auto* AsyncReadHandle = FPlatformFileManager::Get().GetPlatformFile().OpenAsyncRead(*File.AbsoluteFilename);
                                                                                                                       
		const bool bAuditSource = OnProcessChunks.IsBound();
		FAsyncFileCallBack SizeCallback = [&File, AsyncReadHandle, bAuditSource](bool bCancelled, IAsyncReadRequest* Request)
		{
			if (bAuditSource && Request->GetSizeResults() > 16 * 1024 * 1024) File.Module->bCompileError = true;
			if (bCancelled || Request->GetSizeResults() <= 0 || (bAuditSource && Request->GetSizeResults() > 16 * 1024 * 1024))
                                                                                                                     
			{
				File.RawCode = TEXT("");

				FPlatformMisc::MemoryBarrier();
				File.bLoadAsynchronous = false;
			}
			else
			{
				int64 AsyncFileSize = Request->GetSizeResults();
				FAsyncFileCallBack ReadCallback = [&File, AsyncReadHandle, AsyncFileSize](bool bCancelled, IAsyncReadRequest* Request)
				{
					if (!bCancelled)
					{
						uint8* Buffer = Request->GetReadResults();
						FFileHelper::BufferToString(File.RawCode, Buffer, AsyncFileSize);

						FMemory::Free(Buffer);
					}
					else
					{
						File.RawCode = TEXT("");
					}

					FPlatformMisc::MemoryBarrier();
					File.bLoadAsynchronous = false;
				};

				File.AsyncReadRequest = AsyncReadHandle->ReadRequest(
					0, AsyncFileSize,
					EAsyncIOPriorityAndFlags::AIOP_High,
					&ReadCallback
				);
			}
		};

		File.AsyncSizeRequest = AsyncReadHandle->SizeRequest(&SizeCallback);
		File.AsyncReadHandle = AsyncReadHandle;
	}

	// Wait for all files to finish loading asynchronously
	for (FFile& File : Files)
	{
		while (File.bLoadAsynchronous)
		{
			FPlatformProcess::Sleep(0.001f);
			FPlatformMisc::MemoryBarrier();
		}

		delete File.AsyncReadRequest;
		File.AsyncReadRequest = nullptr;

		delete File.AsyncSizeRequest;
		File.AsyncSizeRequest = nullptr;

		delete File.AsyncReadHandle;
		File.AsyncReadHandle = nullptr;
	}
}

                                                                                               
void FAngelscriptPreprocessor::PreloadBlueprintImportAssets()
{
    if (bLoadingAnyFilesAsynchronous) PerformAsynchronousLoads();
    FAngelscriptTypeGenerationContext Context;
    Context.Point = EAngelscriptTypeGenerationPoint::PrepareSource;
    Context.Preprocessor = this;
    Context.bPreloadOnly = true;
    FString Error;
    if (!DispatchAngelscriptTypeGeneration(Context, Error))
        UE_LOG(Angelscript, Fatal, TEXT("%s"), *Error);
}

                                                                                             
bool FAngelscriptPreprocessor::Preprocess()
{
	ConfigSettings = FAngelscriptManager::Get().ConfigSettings;

	FScopedSlowTask SlowTask(1.f, FText::FromString("Script Preprocessing"));
	SlowTask.EnterProgressFrame(1.f);

	FAngelscriptScopeTimer Timer(TEXT("preprocessing"));
	if (!ensureMsgf(!bIsPreprocessed, TEXT("Can only preprocess once.")))
		return false;
	bIsPreprocessed = true;

	// Do asynchronous loads if we queued them
	if (bLoadingAnyFilesAsynchronous)
		PerformAsynchronousLoads();

                                                                                                                     
	const bool bAuditSource = OnProcessChunks.IsBound();
	if (bAuditSource)
		for (FFile& File : Files)
		{
			if (File.Module->bCompileError) bHasError = true;
			File.LoadedSource = File.RawCode;
		}
                                                                                                                   
                                                                                               
    FAngelscriptTypeGenerationContext SourceContext;
    SourceContext.Point = EAngelscriptTypeGenerationPoint::PrepareSource;
    SourceContext.Preprocessor = this;
    FString SourceError;
    if (!DispatchAngelscriptTypeGeneration(SourceContext, SourceError))
    {
        if (!Files.IsEmpty()) LineError(Files[0], 1, SourceError);
        bHasError = true;
        return false;
    }
                                                                                             

                                                                                            
	// Import errors mark their source module; keep preprocessing unrelated units.
	// Generate proxies before wrapping so the top-level import parser sees them,
	// and every generated type remains in its source unit namespace.
	for (FFile& File : Files)
		if (File.Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
			File.RawCode = TEXT("namespace ") + File.Module->ScriptUnit.Id + TEXT(" { ") + File.RawCode + TEXT("\n}");
                                                                                          
                                                                                                           
	FString ExtensionPipelineError;
	if (!DispatchAngelscriptPreParse(*this, ExtensionPipelineError))
	{
		bHasError = true;
		if (Files.Num() != 0)
			FileWideError(Files[0], ExtensionPipelineError);
		return false;
	}
                                                                                                         
	// First step, parse into chunks
	for (FFile& File : Files)
		ParseIntoChunks(File);

                                                                                                        
	// Failed files retain diagnostics and descriptors; other units continue preprocessing.
                                                                                                      

	// Record which classes are created by the angelscript code
                                                                                                                
	for (FFile& File : Files)
		if (!File.Module->bCompileError) DetectClasses(File);
                                                                                                              

                                                                                                         
	// The unit scheduler rejects every file belonging to an erroneous unit.
                                                                                                       

	// Analyze the classes in the files
                                                                                                                 
	for (FFile& File : Files)
		if (!File.Module->bCompileError) AnalyzeClasses(File);
                                                                                                               

	// Process found macros in the chunks
                                                                                                                
	for (FFile& File : Files)
		if (!File.Module->bCompileError) ProcessMacros(File);
                                                                                                              

	// Process any delegate signatures we have
                                                                                                                   
	for (FFile& File : Files)
		if (!File.Module->bCompileError) ProcessDelegates(File);
                                                                                                                 

	OnProcessChunks.Broadcast(*this);
                                                                                                                     
	if (bAuditSource) for (FFile& File : Files) File.LoadedSource.Reset();
                                                                                                                   

	// Process any class default statements
	for (FFile& File : Files)
	{
                                                                                                           
		if (File.Module->bCompileError) continue;
                                                                                                         
		for(FChunk& Chunk : File.ChunkedCode)
		{
			ProcessDefaults(File, Chunk);
		}
	}
	
	// Condense the chunks back into final processed code
                                                                                                                     
	for (FFile& File : Files)
		if (!File.Module->bCompileError) CondenseFromChunks(File);
                                                                                                                   

	// Run post-processing steps
	for (FFile& File : Files)
	{
                                                                                                              
		if (File.Module->bCompileError) continue;
                                                                                                            
		PostProcessRangeBasedFor(File);
		PostProcessLiteralAssets(File);
	}

	OnPostProcessCode.Broadcast(*this);

	// Add the processed code into the module
	for (FFile& File : Files)
	{
		FAngelscriptModuleDesc::FCodeSection Section;
		Section.RelativeFilename = File.RelativeFilename;
		Section.AbsoluteFilename = File.AbsoluteFilename;
		Section.Code = File.ProcessedCode;

		File.Module->Code.Emplace(MoveTemp(Section));
	}

                                                                                                          
	return !bHasError || Files.ContainsByPredicate([](const FFile& File) { return !File.Module->bCompileError; });
                                                                                                        
}

struct FSpecifier
{
	FName Name;
	FString Value;
	TArray<FSpecifier> List;
};

bool IsValidSpecifierIdentifier(const FString& Str)
{
	// Specifier identifiers can be numeric as well
	for (int16 Char : Str)
	{
		if (!FAngelscriptManager::IsValidIdentifierCharacter(Char)
			&& Char != '.' && Char != '-' && Char != '|' && Char != ':')
		{
			return false;
		}
	}

	return !Str.IsEmpty();
}

static TArray<FSpecifier> ParseSpecifiers(const FString& Str, TArray<FString>& OutErrors, int32 Start = 0, int32 End = -1);
static FSpecifier ParseSpecifier(const FString& Str, TArray<FString>& OutErrors, int32 Start = 0, int32 End = -1)
{
	if (End == -1)
		End = Str.Len();

	FSpecifier Spec;

	int EqualsPos = -1;
	int BracketStart = -1;
	int BracketDepth = 0;
	bool bHasList = false;
	bool bInQuotes = false;

	for (int32 Pos = Start; Pos < End; ++Pos)
	{
		int16 Char = Str[Pos];
		switch (Char)
		{
			case '(':
				if(!bInQuotes)
				{
					if (BracketDepth == 0)
						BracketStart = Pos;
					BracketDepth += 1;
				}
			break;
			case ')':
				if (!bInQuotes && BracketDepth > 0)
				{
					BracketDepth -= 1;

					if (BracketDepth == 0)
					{
						bHasList = true;
						Spec.List = ParseSpecifiers(
							Str.Mid(BracketStart+1, Pos-BracketStart-1), OutErrors
						);
						Pos = End;
						break;
					}
				}
			break;
			case '=':
				if (BracketDepth == 0 && !bInQuotes && EqualsPos == -1)
				{
					bool bWasQuoted = false;
					FString NameStr = Str.Mid(Start, Pos-Start).TrimStartAndEnd().TrimQuotes(&bWasQuoted);
					Spec.Name = *NameStr;

					// Error if the specifier contains spaces but wasn't quoted
					if (!bWasQuoted && !IsValidSpecifierIdentifier(NameStr))
						OutErrors.Add(FString::Printf(TEXT("Specifier \"%s\" must be quoted"), *NameStr));

					EqualsPos = Pos;
				}
			break;
			case '"':
				bInQuotes = !bInQuotes;
			break;
			case ' ':
			case '\t':
			break;
			default:
				// We found non-whitespace after an equals that isn't a bracket, treat the whole thing
				// as one specifier and avoid parsing it as a list.
				if (EqualsPos != -1 && BracketDepth == 0)
				{
					Pos = End;
					break;
				}
			break;
		}
	}

	if (!bHasList)
	{
		if (EqualsPos != -1)
		{
			bool bWasQuoted = false;
			FString ValueStr = Str.Mid(EqualsPos+1, End-EqualsPos-1).TrimStartAndEnd().TrimQuotes(&bWasQuoted);
			Spec.Value = ValueStr;

			// Error if the specifier contains spaces but wasn't quoted
			if (!bWasQuoted && !IsValidSpecifierIdentifier(ValueStr))
				OutErrors.Add(FString::Printf(TEXT("%s specifier value \"%s\" must be quoted"), *Spec.Name.ToString(), *ValueStr));
		}
		else
		{
			bool bWasQuoted = false;
			FString NameStr = Str.Mid(Start, End-Start).TrimStartAndEnd().TrimQuotes(&bWasQuoted);
			Spec.Name = FName(*NameStr);

			// Error if the specifier contains spaces but wasn't quoted
			if (!bWasQuoted && !IsValidSpecifierIdentifier(NameStr))
				OutErrors.Add(FString::Printf(TEXT("%s specifier value \"%s\" must be quoted"), *Spec.Name.ToString(), *NameStr));
		}
	}

	return Spec;
}

TArray<FSpecifier> ParseSpecifiers(const FString& Str, TArray<FString>& OutErrors, int32 Start, int32 End)
{
	if (End == -1)
		End = Str.Len();

	int32 BracketDepth = 0;
	TArray<FSpecifier> Specs;

	bool bInQuotes = false;

	int32 TermPos = Start;
	for (int32 Pos = Start; Pos < End; ++Pos)
	{
		int16 Char = Str[Pos];
		switch (Char)
		{
			case '(':
				if(!bInQuotes)
					BracketDepth += 1;
			break;
			case ')':
				if (!bInQuotes)
					BracketDepth -= 1;
			break;
			case '"':
				bInQuotes = !bInQuotes;
			break;
			case ',':
				if (BracketDepth == 0 && !bInQuotes)
				{
					Specs.Add(ParseSpecifier(Str, OutErrors, TermPos, Pos));
					TermPos = Pos + 1;
				}
			break;
		}
	}

	if (TermPos < End)
		Specs.Add(ParseSpecifier(Str, OutErrors, TermPos, End));
	return Specs;
}

static FString GetReturnInit(const FString& ReturnType)
{
	static bool bHaveInitMap = false;
	static TMap<FString, FString> InitMap;

	if (!bHaveInitMap)
	{
		// We need to initialize primitive return values so AS doesn't
		// think we are leaving them uninitialized.
		InitMap.Add(TEXT("bool"), TEXT(" = false"));
		InitMap.Add(TEXT("int"), TEXT(" = 0"));
		InitMap.Add(TEXT("int16"), TEXT(" = 0"));
		InitMap.Add(TEXT("int32"), TEXT(" = 0"));
		InitMap.Add(TEXT("int64"), TEXT(" = 0"));
		InitMap.Add(TEXT("int8"), TEXT(" = 0"));
		InitMap.Add(TEXT("uint"), TEXT(" = 0"));
		InitMap.Add(TEXT("uint16"), TEXT(" = 0"));
		InitMap.Add(TEXT("uint32"), TEXT(" = 0"));
		InitMap.Add(TEXT("uint64"), TEXT(" = 0"));
		InitMap.Add(TEXT("float32"), TEXT(" = 0.f"));
		InitMap.Add(TEXT("float64"), TEXT(" = 0.0"));
		InitMap.Add(TEXT("double"), TEXT(" = 0.0"));

		auto* ConfigSettings = GetMutableDefault<UAngelscriptSettings>();
		if (ConfigSettings->bScriptFloatIsFloat64)
			InitMap.Add(TEXT("float"), TEXT(" = 0.0"));
		else
			InitMap.Add(TEXT("float"), TEXT(" = 0.f"));

		bHaveInitMap = true;
	}

	FString* ReturnInit = InitMap.Find(ReturnType);
	if (ReturnInit != nullptr)
		return *ReturnInit;
	return TEXT("");
}

FString FAngelscriptPreprocessor::GetPushArgumentSuffix(const FString& Type)
{
	if (Type.Contains(TEXT("<")))
	{
		// Template types are always passed using the generic push argument syntax
		return TEXT("");
	}

	FString PushType = Type;
	PushType.RemoveFromStart(TEXT("const "));

	int RefIndex;
	if (PushType.FindLastChar('&', RefIndex))
		PushType = PushType.Mid(0, RefIndex);

	PushType.TrimStartAndEndInline();

	// If we don't have a specialization for this type, call the generic push functions
	if(!FAngelscriptManager::Get().BoundBlueprintEventArgumentSpecializations.Contains(PushType))
	{
		return TEXT("");
	}

	PushType = TEXT("__") + PushType;
	return PushType;
}

void FAngelscriptPreprocessor::ProcessDelegates(FFile& File)
{
	for (FDelegateDesc& Delegate : File.Delegates)
	{
		FString GeneratedCode;

		FChunk& Chunk = File.ChunkedCode[Delegate.ChunkIndex];
		if (Delegate.BracketPos >= Chunk.Content.Len())
			continue;

		int32 NameEnd = Delegate.BracketPos;
		int32 NameStart = NameEnd - 1;
		while (
			(Chunk.Content[NameStart] >= 'a' && Chunk.Content[NameStart] <= 'z')
			|| (Chunk.Content[NameStart] >= 'A' && Chunk.Content[NameStart] <= 'Z')
			|| (Chunk.Content[NameStart] >= '0' && Chunk.Content[NameStart] <= '9')
			|| Chunk.Content[NameStart] == '_')
		{
			NameStart -= 1;
			if (NameStart <= Delegate.StartPosInChunk)
				break;
		}

		FString DelegateName = Chunk.Content.Mid(NameStart+1, NameEnd-NameStart-1);

		auto Desc = MakeShared<FAngelscriptDelegateDesc>();
		Desc->LineNumber = Delegate.FileLineNumber;
		Desc->DelegateName = DelegateName;
		Desc->bIsMulticast = Delegate.bIsMulticast;
		File.Module->Delegates.Add(Desc);

		FMacro FakeMacro;
		FakeMacro.NameStartPos = NameStart-1;
		FakeMacro.NameEndPos = NameEnd;

		GeneratedCode += FString::Printf( TEXT("struct %s {") , *DelegateName);

		if (Delegate.bIsMulticast)
		{
			GeneratedCode += TEXT("_FMulticastScriptDelegate _Inner;");
		}
		else
		{
			GeneratedCode += TEXT("_FScriptDelegate _Inner;");
		}

		GeneratedCode += FString::Printf(TEXT("%s() __generated no_discard {}"), *DelegateName);
		GeneratedCode += FString::Printf(TEXT("%s(const %s& Other) __generated no_discard { this = Other; }"), *DelegateName, *DelegateName);
		GeneratedCode += FString::Printf(TEXT("%s& opAssign(const %s& Other) __generated { _Inner = Other._Inner; return this; }"), *DelegateName, *DelegateName);

		TArray<FString> ArgumentNames;
		TArray<FString> ArgumentTypes;
		bool bConstMethod = false;
		bool bPropertyMethod = false;

		FString Arguments = ExtractArgumentList(Chunk, FakeMacro, ArgumentNames, ArgumentTypes, bConstMethod, bPropertyMethod);

		bool bReturnIsConst = false;
		FString AccessSpecifier;
		FString ReturnType = ExtractReturnType(Chunk, FakeMacro, bReturnIsConst, AccessSpecifier);

		FString QualifiedReturnType = ReturnType;
		if (bReturnIsConst)
			QualifiedReturnType = TEXT("const ") + ReturnType;

		ReturnType.RemoveFromStart(TEXT("delegate"));
		ReturnType.RemoveFromStart(TEXT("event"));

		FString PushArgumentCode;
		for (int32 ArgIndex = 0, ArgCount = ArgumentNames.Num(); ArgIndex < ArgCount; ++ArgIndex)
		{
			const FString& Arg = ArgumentNames[ArgIndex];
			const bool bIsRefArgument = ArgumentTypes[ArgIndex].Contains(TEXT("&"));
			if (bIsRefArgument)
			{
				const bool bIsConstRefArgument = ArgumentTypes[ArgIndex].Contains(TEXT("const "));
				if (!bIsConstRefArgument)
				{
					PushArgumentCode += FString::Printf(TEXT(" __Evt_PushArgumentRef%s(%s);"),
						*GetPushArgumentSuffix(ArgumentTypes[ArgIndex]),
						*Arg);
					continue;
				}
			}

			PushArgumentCode += FString::Printf(TEXT(" __Evt_PushArgument%s(%s);"),
				*GetPushArgumentSuffix(ArgumentTypes[ArgIndex]),
				*Arg);
		}

		bool bHaveReturn = ReturnType != TEXT("void");

		if (Delegate.bIsMulticast)
		{
			GeneratedCode += FString::Printf(TEXT("%s Broadcast(%s) const __generated unsafe_during_construction {"), *QualifiedReturnType, *Arguments);
			GeneratedCode += TEXT("if (!_Inner.IsBound()) return;");
			GeneratedCode += PushArgumentCode;
			GeneratedCode += TEXT(" __Evt_ExecuteDelegate(_Inner);");
			GeneratedCode += TEXT("}");

			GeneratedCode += TEXT("void AddUFunction(const UObject Object, const FName& FunctionName) __generated unsafe_during_construction { _Inner.__Internal_AddUFunction(Object, FunctionName, __DelegateSignature(this)); }");

			//GeneratedCode += TEXT("void Unbind(UObject Object, const FName& FunctionName) __generated unsafe_during_construction { _Inner.Unbind(Object, FunctionName); }");
			//GeneratedCode += TEXT("void UnbindObject(UObject Object) __generated unsafe_during_construction { _Inner.UnbindObject(Object); }");
		}
		else
		{
			FString GeneratedReturn;
			FString GeneratedBody;

			if (bHaveReturn)
			{
				GeneratedReturn += FString::Printf(TEXT(" %s __ReturnValue%s;"),
					*ReturnType, *GetReturnInit(ReturnType));
			}

			GeneratedBody += PushArgumentCode;

			if (bHaveReturn)
			{
				GeneratedBody += FString::Printf(TEXT("__Evt_PushArgumentRef%s(__ReturnValue);"),
					*GetPushArgumentSuffix(ReturnType));
			}

			GeneratedBody += TEXT(" __Evt_ExecuteDelegate(_Inner);");
			if (bHaveReturn)
				GeneratedBody += TEXT(" return __ReturnValue;");
			GeneratedBody += TEXT("}");

			GeneratedCode += FString::Printf(TEXT("%s Execute(%s) const allow_discard __generated unsafe_during_construction {"), *QualifiedReturnType, *Arguments);
			GeneratedCode += GeneratedReturn;
			if (bHaveReturn)
				GeneratedCode += TEXT("if (!_Inner.IsBound()) { Throw(\"Executing unbound delegate.\"); return __ReturnValue; }");
			else
				GeneratedCode += TEXT("if (!_Inner.IsBound()) { Throw(\"Executing unbound delegate.\"); return; }");
			GeneratedCode += GeneratedBody;

			GeneratedCode += FString::Printf(TEXT("%s ExecuteIfBound(%s) const allow_discard __generated unsafe_during_construction {"), *QualifiedReturnType, *Arguments);
			GeneratedCode += GeneratedReturn;
			if (bHaveReturn)
				GeneratedCode += TEXT("if (!_Inner.IsBound()) { return __ReturnValue; }");
			else
				GeneratedCode += TEXT("if (!_Inner.IsBound()) { return; }");
			GeneratedCode += GeneratedBody;

			GeneratedCode += TEXT("void BindUFunction(UObject Object, const FName& BindFunctionName) __generated unsafe_during_construction { _Inner.__Internal_BindUFunction(Object, BindFunctionName, __DelegateSignature(this)); }");
			//GeneratedCode += TEXT("UObject GetUObject() const property __generated { return _Inner.GetUObject(); }");
			//GeneratedCode += TEXT("FName GetFunctionName() const property __generated { return _Inner.GetFunctionName(); }");

			GeneratedCode += FString::Printf(TEXT("%s(UObject Object, const FName& BindFunctionName) __generated no_discard unsafe_during_construction { _Inner.__Internal_BindUFunction(Object, BindFunctionName, __DelegateSignature(this)); }"), *DelegateName);
		}

		GeneratedCode +=
			//TEXT("bool IsBound() const __generated { return _Inner.IsBound(); }")
			//TEXT("void Clear() __generated { _Inner.Clear(); }")
			TEXT("};");

		File.GeneratedCode.Add(GeneratedCode);

		ReplaceWithBlank(File.ChunkedCode[Delegate.ChunkIndex], Delegate.StartPosInChunk, Delegate.EndPosInChunk+1);
	}
}

void FAngelscriptPreprocessor::DetectClasses(FFile& File)
{
	for (FChunk& Chunk : File.ChunkedCode)
	{
		if (Chunk.Type == EChunkType::Class)
			DetectClasses(File, Chunk);
		else if (Chunk.Type == EChunkType::Struct)
			DetectClasses(File, Chunk);
		else if (Chunk.Type == EChunkType::Enum)
			DetectEnum(File, Chunk);
	}
}

bool FAngelscriptPreprocessor::IsPreprocessingModule(const FString& ModuleName)
{
	for (auto& File : Files)
	{
		if (File.Module.IsValid() && File.Module->ModuleName == ModuleName)
			return true;
	}
	return false;
}

static FName PP_NAME_ToolTip("ToolTip");
void FAngelscriptPreprocessor::DetectClasses(FFile& File, FChunk& Chunk)
{
	static const FRegexPattern ClassPattern(TEXT("(class|struct)\\s+([A-Za-z0-9_]+)(\\s*:\\s*([A-Za-z0-9_]+\\s*::\\s*)*([A-Za-z0-9_]+))?"));

	FRegexMatcher MatchClass(ClassPattern, Chunk.Content);
	if (!MatchClass.FindNext())
		return;

	FString ClassName = MatchClass.GetCaptureGroup(2);

	// Add the class into the output module
	TSharedRef<FAngelscriptClassDesc> ClassDesc = MakeShared<FAngelscriptClassDesc>();
	ClassDesc->LineNumber = Chunk.FileLineNumber;
	ClassDesc->ClassName = ClassName;
	ClassDesc->Namespace = Chunk.Namespace;
                                                                                                   
	if (File.Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
		ClassDesc->QualifiedName = ClassDesc->Namespace.Get(File.Module->ScriptUnit.Id) + TEXT("::") + ClassName;
                                                                                                 

#if WITH_EDITOR
	if (Chunk.Comment.Len() != 0)
		ClassDesc->Meta.Add(PP_NAME_ToolTip, FormatCommentForToolTip(Chunk.Comment));
#endif

	if (Chunk.Type == EChunkType::Struct)
		ClassDesc->bIsStruct = true;

	// Determine the direct superclass of this type
	ClassDesc->SuperClass = MatchClass.GetCaptureGroup(5);
                                                                                                  
	if (!MatchClass.GetCaptureGroup(4).IsEmpty())
		ClassDesc->SuperClass = MatchClass.GetCaptureGroup(4).Replace(TEXT(" "), TEXT("")).Replace(TEXT("\t"), TEXT("")) + ClassDesc->SuperClass;
                                                                                                
	if (ClassDesc->SuperClass.Len() == 0)
	{
		if (!ClassDesc->bIsStruct)
		{
			// No superclass specified on a non-struct means this is a type of UObject
			ClassDesc->SuperClass = TEXT("UObject");
		}
	}

	Chunk.ClassDesc = ClassDesc;
	File.Module->Classes.Add(ClassDesc);

	// Make sure that a class with this name is valid to add
	TSharedPtr<FAngelscriptClassDesc> ExistingClass;
	TSharedPtr<FAngelscriptModuleDesc> ExistingModule;

                                                                                                     
	ExistingClass = FAngelscriptManager::Get().GetClass(ClassDesc->QualifiedName.IsEmpty() ? ClassName : ClassDesc->QualifiedName, &ExistingModule);
                                                                                                   

	if (ExistingClass.IsValid())
	{
		// If we're not preprocessing this module, and the class already exists, error.
		if (!IsPreprocessingModule(ExistingModule->ModuleName))
		{
			ChunkError(File, Chunk, FString::Printf(TEXT("Cannot declare class %s in module %s. A class with this name already exists in module %s."),
				*ClassName, *File.Module->ModuleName, *ExistingModule->ModuleName));
			bHasError = true;
		}
		else
		{
			// Check if another module we're preprocessing already contains this class
			for (auto& OtherFile : Files)
			{
				if (OtherFile.Module == File.Module)
					continue;

                                                                                                      
				if (OtherFile.Module->ScriptUnit.Id == File.Module->ScriptUnit.Id && OtherFile.Module->GetClass(ClassName).IsValid())
                                                                                                    
				{
					ChunkError(File, Chunk, FString::Printf(TEXT("Cannot declare class %s in module %s. A class with this name already exists in module %s."),
						*ClassName, *File.Module->ModuleName, *OtherFile.Module->ModuleName));
					bHasError = true;
				}
			}
		}
	}

                                                                                                                   
	PreprocessingClasses.Add(ClassDesc->QualifiedName.IsEmpty() ? ClassDesc->ClassName : ClassDesc->QualifiedName, ClassDesc);
                                                                                                                 

	// Genarate the script chunk containing the static class global variable that will be set
	// to the correct class by AngelscriptClassGenerator later.
	if (Chunk.Type == EChunkType::Class)
	{
		FString ClassVar = FString::Printf(TEXT("__StaticType_%s"), *ClassDesc->ClassName);
		ClassDesc->StaticClassGlobalVariableName = ClassVar;

		if (ConfigSettings->StaticClassDeprecation == EAngelscriptDeprecationMode::Disallowed)
		{
			if (ClassDesc->Namespace.IsSet())
			{
				File.GeneratedCode.Add(FString::Printf(TEXT("namespace %s { const TSubclassOf<UObject> %s; }"),
					*ClassDesc->Namespace.GetValue(), *ClassVar));
			}
			else
			{
				File.GeneratedCode.Add(FString::Printf(TEXT("const TSubclassOf<UObject> %s;"), *ClassVar));
			}
		}
		else
		{
			FString FunctionSpecifiers;
			if (ConfigSettings->StaticClassDeprecation == EAngelscriptDeprecationMode::Deprecated)
				FunctionSpecifiers += TEXT(" deprecated");

			if (ClassDesc->Namespace.IsSet())
			{
				File.GeneratedCode.Add(FString::Printf(TEXT("namespace %s { const TSubclassOf<UObject> %s; namespace %s { UClass StaticClass() __generated%s { return %s; } } }"),
					*ClassDesc->Namespace.GetValue(), *ClassVar, *ClassDesc->ClassName, *FunctionSpecifiers, *ClassVar));
			}
			else
			{
				File.GeneratedCode.Add(FString::Printf(TEXT("const TSubclassOf<UObject> %s; namespace %s { UClass StaticClass() __generated%s { return %s; } }"),
					*ClassVar, *ClassDesc->ClassName, *FunctionSpecifiers, *ClassVar));
			}
		}
	}
	else if (Chunk.Type == EChunkType::Struct)
	{
		FString ClassVar = FString::Printf(TEXT("__StaticType_%s"), *ClassDesc->ClassName);
		ClassDesc->StaticClassGlobalVariableName = ClassVar;

		if (ClassDesc->Namespace.IsSet())
		{
			File.GeneratedCode.Add(FString::Printf(TEXT("namespace %s { const TStructType<FScriptStructWildcard> %s; }"),
				*ClassDesc->Namespace.GetValue(), *ClassVar));
		}
		else
		{
			File.GeneratedCode.Add(FString::Printf(TEXT("const TStructType<FScriptStructWildcard> %s;"), *ClassVar));
		}
	}
}

TSharedPtr<FAngelscriptClassDesc> FAngelscriptPreprocessor::GetOrCreateStaticsClass(FFile& File)
{
	if (File.StaticsClass.IsValid())
		return File.StaticsClass;

	FString ClassName = TEXT("Module_");
	ClassName += MakeIdentifier(File.Module->ModuleName);
	ClassName += TEXT("Statics");

	TSharedRef<FAngelscriptClassDesc> ClassDesc = MakeShared<FAngelscriptClassDesc>();
	ClassDesc->ClassName = ClassName;
	ClassDesc->SuperClass = TEXT("UObject");
	ClassDesc->bIsStaticsClass = true;
	ClassDesc->bSuperIsCodeClass = true;
	ClassDesc->CodeSuperClass = UObject::StaticClass();
	ClassDesc->Meta.Add(TEXT("DisplayName"), FPaths::GetBaseFilename(File.RelativeFilename));

	File.Module->Classes.Add(ClassDesc);
                                                                                                                  
	if (File.Module->ScriptUnit.Layer == EAngelscriptScriptLayer::Leaf)
	{
		ClassDesc->Namespace = File.Module->ScriptUnit.Id;
		ClassDesc->QualifiedName = File.Module->ScriptUnit.Id + TEXT("::") + ClassDesc->ClassName;
	}
	PreprocessingClasses.Add(ClassDesc->QualifiedName.IsEmpty() ? ClassDesc->ClassName : ClassDesc->QualifiedName, ClassDesc);
                                                                                                                

	File.StaticsClass = ClassDesc;
	return ClassDesc;
}

static FName PP_NAME_EditorOnly("EditorOnly");

// Function partially copied from FAngelscriptFunctionSignature::GetScriptNamespaceForClass, to be kept synchronized in case of changes
static FString GetScriptNameFromClassName(const FString& OriginalClassName)
{
	FString ClassName = OriginalClassName;

	bool bFoundPrefix = false;
	for (const auto& Prefix : FAngelscriptManager::BlueprintLibraryNamespacePrefixesToStrip)
	{
		if (ClassName.RemoveFromStart(Prefix))
		{
			bFoundPrefix = true;
			break;
		}
	}

	bool bFoundSuffix = false;
	for (const auto& Suffix : FAngelscriptManager::BlueprintLibraryNamespaceSuffixesToStrip)
	{
		if (ClassName.RemoveFromEnd(Suffix))
		{
			bFoundSuffix = true;
			break;
		}
	}

	if (!bFoundPrefix && bFoundSuffix)
	{
#if WITH_ANGELSCRIPT_HAZE
		ClassName.RemoveFromStart("UHaze");
#endif

		ClassName.RemoveFromStart("U");
	}

	return ClassName;
}

void FAngelscriptPreprocessor::AnalyzeClasses(FFile& File)
{
                                                                                                
	TGuardValue<const FAngelscriptScriptUnit*> Scope(FAngelscriptManager::Get().CurrentScriptUnit, &File.Module->ScriptUnit);
                                                                                              
	for (FChunk& Chunk : File.ChunkedCode)
	{
		if (Chunk.Type == EChunkType::Class)
			AnalyzeClasses(File, Chunk);
		else if (Chunk.Type == EChunkType::Struct)
			AnalyzeStructs(File, Chunk);
	}
}

void FAngelscriptPreprocessor::AnalyzeClasses(FFile& File, FChunk& Chunk)
{
	auto ClassDesc = Chunk.ClassDesc;
	if (!ClassDesc.IsValid())
		return;

	// Determine our first code class parent
	ResolveSuperClass(ClassDesc);

	OnModifyClassDefaults.Broadcast(*this, ClassDesc);

	// If inheriting from a code class we should remove the inheritance from the script
	if (ClassDesc->bSuperIsCodeClass)
	{
		static const FRegexPattern ClassPattern(TEXT("(class|struct)\\s+([A-Za-z0-9_]+)(\\s*:\\s*[A-Za-z0-9_]+)?"));

		FRegexMatcher MatchClass(ClassPattern, Chunk.Content);
		if (!ensureMsgf(MatchClass.FindNext(), TEXT("Class code chunk did not include a valid class declaration???")))
			return;

		int32 InheritBeginPos = MatchClass.GetCaptureGroupBeginning(3);
		int32 InheritEndPos = MatchClass.GetCaptureGroupEnding(3);

		if (InheritBeginPos != -1)
		{
			for (int32 Pos = InheritBeginPos; Pos < InheritEndPos; ++Pos)
				Chunk.Content[Pos] = ' ';
		}
	}

	if (File.bEditorOnlyModule)
	{
		ClassDesc->Meta.Add(PP_NAME_EditorOnly, TEXT(""));
	}

	// Add some auto-generated static functions for the class type
	if (Chunk.Type == EChunkType::Class && ClassDesc->CodeSuperClass != nullptr)
	{
		FString ClassNamespace;
		if (ClassDesc->Namespace.IsSet())
			ClassNamespace = ClassDesc->Namespace.GetValue() + TEXT("::");

		FString GeneratedStatics;
		GeneratedStatics += FString::Printf(TEXT("namespace %s%s {"), *ClassNamespace, *ClassDesc->ClassName);

		bool bHasStatics = false;
		if (ConfigSettings->DeprecateOldActorGenericMethods != EAngelscriptDeprecationMode::Disallowed
			&& ClassDesc->CodeSuperClass->IsChildOf(AActor::StaticClass()))
		{
			bool bDeprecated = false;
			if (ConfigSettings->DeprecateOldActorGenericMethods == EAngelscriptDeprecationMode::Deprecated)
				bDeprecated = true;

			GeneratedStatics += FString::Printf(
				TEXT("\n %s Spawn(const FVector& Location = FVector::ZeroVector,")
				TEXT(" const FRotator& Rotation = FRotator::ZeroRotator,")
				TEXT(" const FName& Name = NAME_None, bool bDeferredSpawn = false, ULevel Level = nullptr) __generated %s {")
				TEXT("return Cast<%s>(SpawnActor(%s.Get(), Location, Rotation, Name, bDeferredSpawn, Level));")
				TEXT("}"),
				*ClassDesc->ClassName,
				bDeprecated ? TEXT("deprecated") : TEXT(""),
				*ClassDesc->ClassName,
				*ClassDesc->StaticClassGlobalVariableName
			);

			bHasStatics = true;
		}
		else if (ClassDesc->CodeSuperClass->IsChildOf(UActorComponent::StaticClass()))
		{
			GeneratedStatics += FString::Printf(
				TEXT("\n %s Get(const AActor Actor, FName WithName = NAME_None) __generated no_discard {")
				TEXT("%s Value; __Actor_GetComponentByClass(Actor, %s, Value, WithName); return Value;")
				TEXT("}"),
				*ClassDesc->ClassName,
				*ClassDesc->ClassName,
				*ClassDesc->StaticClassGlobalVariableName
			);

			GeneratedStatics += FString::Printf(
				TEXT("\n %s GetOrCreate(AActor Actor, FName WithName = NAME_None) __generated {")
				TEXT("%s Value; __Actor_GetOrCreateComponentByClass(Actor, %s, Value, WithName); return Value;")
				TEXT("}"),
				*ClassDesc->ClassName,
				*ClassDesc->ClassName,
				*ClassDesc->StaticClassGlobalVariableName
			);

			GeneratedStatics += FString::Printf(
				TEXT("\n %s Create(AActor Actor, FName WithName = NAME_None) __generated {")
				TEXT("%s Value; __Actor_CreateComponentByClass(Actor, %s, Value, WithName); return Value;")
				TEXT("}"),
				*ClassDesc->ClassName,
				*ClassDesc->ClassName,
				*ClassDesc->StaticClassGlobalVariableName
			);

			bHasStatics = true;
		}
		else if (ClassDesc->CodeSuperClass->IsChildOf(USubsystem::StaticClass()))
		{
			if (ClassDesc->CodeSuperClass->IsChildOf(UEngineSubsystem::StaticClass()))
			{
				bHasStatics = true;

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get() __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetEngineSubsystem(%s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("USubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);
			}
	#if WITH_EDITOR
			else if (ClassDesc->CodeSuperClass->IsChildOf(UEditorSubsystem::StaticClass()))
			{
				bHasStatics = true;

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get() __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetEditorSubsystem(%s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("UEditorSubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);
			}
	#endif
			else if (ClassDesc->CodeSuperClass->IsChildOf(UGameInstanceSubsystem::StaticClass()))
			{
				bHasStatics = true;

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get() __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetGameInstanceSubsystem(%s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("USubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);
			}
			else if (ClassDesc->CodeSuperClass->IsChildOf(UWorldSubsystem::StaticClass()))
			{
				bHasStatics = true;

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get() __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetWorldSubsystem(%s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("USubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);
			}
			else if (ClassDesc->CodeSuperClass->IsChildOf(ULocalPlayerSubsystem::StaticClass()))
			{
				bHasStatics = true;

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get(ULocalPlayer LocalPlayer) __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetLocalPlayerSubsystemFromLocalPlayer(LocalPlayer, %s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("USubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);

				GeneratedStatics += FString::Printf(
					TEXT("\n %s Get(APlayerController PlayerController) __generated no_discard {")
					TEXT("return Cast<%s>(%s::GetLocalPlayerSubsystemFromPlayerController(PlayerController, %s.Get()));")
					TEXT("}"),
					*ClassDesc->ClassName,
					*ClassDesc->ClassName,
					*GetScriptNameFromClassName(TEXT("USubsystemLibrary")),
					*ClassDesc->StaticClassGlobalVariableName
				);
			}
		}

		if(FAngelscriptCodeModule::GetClassAnalyze().IsBound())
			FAngelscriptCodeModule::GetClassAnalyze().Execute(GeneratedStatics, ClassDesc, bHasStatics);

		GeneratedStatics += TEXT("}");

		if (bHasStatics)
			File.GeneratedCode.Add(GeneratedStatics);
	}
}

void FAngelscriptPreprocessor::AnalyzeStructs(FFile& File, FChunk& Chunk)
{
	auto ClassDesc = Chunk.ClassDesc;
	if (!ClassDesc.IsValid())
		return;

	// Structs cannot inherit from anything
	if (ClassDesc->SuperClass.Len() != 0)
	{
		ChunkError(File, Chunk, FString::Printf(TEXT("Error parsing script struct %s. Structs may not inherit from anything."), *ClassDesc->ClassName));
		bHasError = true;
	}

	if (File.bEditorOnlyModule)
	{
		ClassDesc->Meta.Add(PP_NAME_EditorOnly, TEXT(""));
	}

	OnModifyClassDefaults.Broadcast(*this, ClassDesc);
}

void FAngelscriptPreprocessor::ProcessMacros(FFile& File)
{
	for (FChunk& Chunk : File.ChunkedCode)
	{
		for (FMacro& Macro : Chunk.Macros)
		{
			if (Macro.Type == EMacroType::Function)
				ProcessFunctionMacro(File, Chunk, Macro);
			else if (Macro.Type == EMacroType::Property)
				ProcessPropertyMacro(File, Chunk, Macro);
			else if (Macro.Type == EMacroType::Class)
				ProcessClassMacro(File, Chunk, Macro);
		}
	}
}

void FAngelscriptPreprocessor::ProcessDefaults(FFile& File, FChunk& Chunk)
{
	if (Chunk.Defaults.Num() == 0)
		return;

	ProcessReplacements(File, Chunk);

	FString GeneratedDefaults;
	
	int32 PlacementStart = -1;
	int32 PlacementEnd = -1;

	for (FDefaultsCode& Code : Chunk.Defaults)
	{
		int32 EndPos = Code.StartPos;
		while (EndPos < Chunk.Content.Len() && Chunk.Content[EndPos] != '\n')
			++EndPos;

		FString DefaultsLine = Chunk.Content.Mid(Code.StartPos + 8, EndPos - Code.StartPos - 8).TrimStartAndEnd();
		StripCommentsFromLine(DefaultsLine);
		GeneratedDefaults += DefaultsLine;
	}

	Chunk.ClassDesc->DefaultsCode = GeneratedDefaults;
}


void FAngelscriptPreprocessor::ReplaceWithBlank(FChunk& Chunk, int32 StartPos, int32 EndPos)
{
	if (!Chunk.Content.IsValidIndex(StartPos) || !Chunk.Content.IsValidIndex(EndPos-1))
		return;
	for (int32 Pos = StartPos; Pos < EndPos; ++Pos)
	{
		if (!IsWhitespace(Chunk.Content[Pos]))
			Chunk.Content[Pos] = ' ';
	}
}

void FAngelscriptPreprocessor::RemoveReplacementsInRange(FChunk& Chunk, int32 StartPos, int32 EndPos)
{
	for (int i = Chunk.Replacements.Num() - 1; i >= 0; --i)
	{
		if (Chunk.Replacements[i].StartPos >= StartPos && Chunk.Replacements[i].EndPos <= EndPos)
		{
			Chunk.Replacements.RemoveAt(i);
		}
	}
}

void FAngelscriptPreprocessor::ReplaceInChunk(FChunk& Chunk, int32 StartPos, int32 EndPos, const FString& Replacement)
{
	Chunk.Replacements.Add({StartPos, EndPos, Replacement});
}

static FName PP_NAME_BlueprintCallable("BlueprintCallable");
static FName PP_NAME_NotBlueprintCallable("NotBlueprintCallable");
static FName PP_NAME_BlueprintPure("BlueprintPure");
static FName PP_NAME_BlueprintEvent("BlueprintEvent");
static FName PP_NAME_NetFunction("NetFunction");
static FName PP_NAME_CrumbFunction("CrumbFunction");
static FName PP_NAME_NetMulticast("NetMulticast");
static FName PP_NAME_WithValidation("WithValidation");
static FName PP_NAME_NetClient("Client");
static FName PP_NAME_NetServer("Server");
static FName PP_NAME_Unreliable("Unreliable");
static FName PP_NAME_BlueprintOverride("BlueprintOverride");
static FName PP_NAME_Meta("Meta");
static FName PP_NAME_DisplayName("DisplayName");
static FName PP_NAME_Keywords("Keywords");
static FName PP_NAME_Category("Category");
static FName PP_NAME_DevFunction("DevFunction");
static FName PP_NAME_CallInEditor("CallInEditor");
static FName PP_NAME_ForcedAssets("ForcedAssets");
static FName PP_NAME_BlueprintAuthorityOnly("BlueprintAuthorityOnly");
static FName PP_NAME_Exec("Exec");
static FName PP_NAME_BlueprintProtected("BlueprintProtected");
static FName PP_NAME_BlueprintSetter("BlueprintSetter");
static FName PP_NAME_BlueprintGetter("BlueprintGetter");

void FAngelscriptPreprocessor::ProcessFunctionMacro(FFile& File, FChunk& Chunk, FMacro& Macro)
{
	// Create the function descriptor
	auto FunctionDesc = MakeShared<FAngelscriptFunctionDesc>();
	FunctionDesc->LineNumber = Macro.FileLineNumber;

	TSharedPtr<FAngelscriptClassDesc> ClassDesc;
	if (Chunk.Type == EChunkType::Global)
	{
		// Global functions should go in a specially generated static class
		FunctionDesc->bIsStatic = true;
		ClassDesc = GetOrCreateStaticsClass(File);
	}
	else
	{
		ClassDesc = Chunk.ClassDesc;
		if (!ClassDesc.IsValid())
		{
			MacroError(File, Macro, TEXT("Error: Method is not inside a valid class"));
			return;
		}
	}

	ClassDesc->Methods.Add(FunctionDesc);

	// Structs cannot have functions
	if (ClassDesc->bIsStruct)
	{
		MacroError(File, Macro, FString::Printf(TEXT("Error parsing script struct %s. Structs may not have any UFUNCTION()s."), *ClassDesc->ClassName));
		bHasError = true;
	}

	FunctionDesc->bBlueprintCallable = bDefaultFunctionBlueprintCallable;
	FunctionDesc->FunctionName = Macro.Name;
	FunctionDesc->ScriptFunctionName = Macro.Name;

	if (Macro.bEditorOnly || File.bEditorOnlyModule)
		FunctionDesc->Meta.Add(PP_NAME_EditorOnly, TEXT(""));

#if WITH_EDITOR
	if(Macro.Comment.Len() != 0)
		FunctionDesc->Meta.Add(PP_NAME_ToolTip, FormatCommentForToolTip(Macro.Comment));
#endif

	bool bHadNotCallable = false;
	bool bHadCallable = false;

	OnModifyFunctionDefaults.Broadcast(*this, ClassDesc, FunctionDesc);

	TArray<FString> SpecifierParseErrors;
	TArray<FSpecifier> Specs = ParseSpecifiers(Macro.Arguments, SpecifierParseErrors);
	for (FString& Error : SpecifierParseErrors)
	{
		MacroError(File, Macro, Error);
		bHasError = true;
	}

	for (auto& Spec : Specs)
	{
		if (Spec.Name == PP_NAME_BlueprintCallable)
		{
			FunctionDesc->bBlueprintCallable = true;
			bHadCallable = true;
		}
		else if (Spec.Name == PP_NAME_NotBlueprintCallable)
		{
			FunctionDesc->bBlueprintCallable = false;
			bHadNotCallable = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintPure)
		{
			FunctionDesc->bBlueprintCallable = true;
			FunctionDesc->bBlueprintPure = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintEvent)
		{
			if (FunctionDesc->bIsStatic)
			{
				MacroError(File, Macro, FString::Printf(TEXT("Global UFUNCTION() %s may not be marked BlueprintEvent."), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			if (FunctionDesc->bBlueprintOverride)
			{
				MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s cannot be both BlueprintEvent and BlueprintOverride."), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			if (FunctionDesc->bNetClient || FunctionDesc->bNetMulticast || FunctionDesc->bNetServer)
			{
				MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s cannot both be a blueprint event and have network specifiers, this is not supported by Unreal"), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			bool bAlreadyHasWrapper = FunctionDesc->bBlueprintEvent;

			if (!bHadCallable)
				FunctionDesc->bBlueprintCallable = false;
			FunctionDesc->bBlueprintEvent = true;
			FunctionDesc->bCanOverrideEvent = true;

			if (!bAlreadyHasWrapper)
			{
				// Generate the blueprint event caller wrapper function
				GenerateBlueprintEventWrapper(File, Chunk, Macro, FunctionDesc);

				// Suffix the script function name so it doesn't collide
				FunctionDesc->ScriptFunctionName += TEXT("_Implementation");
			}
		}
#if WITH_ANGELSCRIPT_HAZE
		else if (Spec.Name == PP_NAME_NetFunction || Spec.Name == PP_NAME_CrumbFunction)
		{
			if (FunctionDesc->bIsStatic)
			{
				MacroError(File, Macro, "Static UFUNCTION()s cannot be NetFunction");
				bHasError = true;
				continue;
			}

			if (FunctionDesc->bBlueprintOverride)
			{
				MacroError(File, Macro, "UFUNCTION() %s cannot be both NetFunction and BlueprintOverride");
				bHasError = true;
				continue;
			}

			bool bAlreadyHasWrapper = FunctionDesc->bBlueprintEvent;

			if (!bHadNotCallable)
				FunctionDesc->bBlueprintCallable = true;
			if (Spec.Name == PP_NAME_CrumbFunction)
				FunctionDesc->Meta.Add(Spec.Name, FString());

			FunctionDesc->bBlueprintEvent = true;
			FunctionDesc->bNetFunction = true;

			if (!bAlreadyHasWrapper)
			{
				// Set it as not blueprint overridable unless we also have BlueprintEvent
				FunctionDesc->bCanOverrideEvent = false;

				// Generate the blueprint event caller wrapper function
				GenerateBlueprintEventWrapper(File, Chunk, Macro, FunctionDesc);

				// Suffix the script function name so it doesn't collide
				FunctionDesc->ScriptFunctionName += TEXT("_Implementation");
			}
		}
		else if (Spec.Name == PP_NAME_DevFunction)
		{
			FunctionDesc->bDevFunction = true;
		}
#else
		else if (Spec.Name == PP_NAME_NetMulticast || Spec.Name == PP_NAME_NetServer || Spec.Name == PP_NAME_NetClient)
		{
			if (FunctionDesc->bBlueprintOverride || FunctionDesc->bBlueprintEvent)
			{
				MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s cannot both be a blueprint event and have network specifiers, this is not supported by Unreal"), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			if (FunctionDesc->bIsStatic)
			{
				MacroError(File, Macro, TEXT("Static UFUNCTION()s cannot use network specifiers"));
				bHasError = true;
				continue;
			}

			bool bAlreadyHasWrapper = FunctionDesc->bBlueprintEvent;

			if (!bHadNotCallable)
				FunctionDesc->bBlueprintCallable = true;

			FunctionDesc->bBlueprintEvent = true;
			FunctionDesc->bNetMulticast = Spec.Name == PP_NAME_NetMulticast;
			FunctionDesc->bNetClient = Spec.Name == PP_NAME_NetClient;
			FunctionDesc->bNetServer = Spec.Name == PP_NAME_NetServer;

#if AS_ENFORCE_SERVER_RPC_VALIDATION
			if (FunctionDesc->bNetServer)
			{
				// Ensure the function also has the WithValidation property
				if (!Specs.FindByPredicate([](FSpecifier& CurSpec) -> bool { return CurSpec.Name == PP_NAME_WithValidation; }))
				{
					MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s is marked as Server but does not have the WithValidation property specified!"), *FunctionDesc->FunctionName));
					bHasError = true;
					continue;
				}
			}
#endif

			if (!bAlreadyHasWrapper)
			{
				// Set it as not blueprint overridable unless we also have BlueprintEvent
				FunctionDesc->bCanOverrideEvent = false;

				// Generate the blueprint event caller wrapper function
				GenerateBlueprintEventWrapper(File, Chunk, Macro, FunctionDesc);

				// Suffix the script function name so it doesn't collide
				FunctionDesc->ScriptFunctionName += TEXT("_Implementation");
			}
		}
		else if (Spec.Name == PP_NAME_WithValidation)
		{
			// Make sure the function also has the Server or Client property
			if (Specs.FindByPredicate([](FSpecifier& CurSpec) -> bool { return CurSpec.Name == PP_NAME_NetServer || CurSpec.Name == PP_NAME_NetClient; }))
			{
				FunctionDesc->bNetValidate = true;
			}
			else
			{
				MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s has the WithValidation property without the Server or Client property!"), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}
		}
		else if (Spec.Name == PP_NAME_BlueprintAuthorityOnly)
		{
			FunctionDesc->bBlueprintAuthorityOnly = true;
		}
#endif
		else if (Spec.Name == PP_NAME_Exec)
		{
			FunctionDesc->bExec = true;
		}
		else if (Spec.Name == PP_NAME_Unreliable)
		{
			FunctionDesc->bUnreliable = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintOverride)
		{
			if (FunctionDesc->bIsStatic)
			{
				MacroError(File, Macro, FString::Printf(TEXT("Global UFUNCTION() %s may not be BlueprintOverride."), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			if (FunctionDesc->bBlueprintEvent)
			{
				MacroError(File, Macro, FString::Printf(TEXT("UFUNCTION() %s cannot be both BlueprintEvent and BlueprintOverride."), *FunctionDesc->FunctionName));
				bHasError = true;
				continue;
			}

			if (!bHadCallable)
				FunctionDesc->bBlueprintCallable = false;

			FunctionDesc->bBlueprintEvent = true;
			FunctionDesc->bBlueprintOverride = true;

			// Don't need a wrapper here, wrapper will already be
			// inherited from the parent class, whether it is AS or Code.

			// Suffix the script function name so it doesn't collide
			FunctionDesc->ScriptFunctionName += TEXT("_Implementation");
		}
		else if (Spec.Name == PP_NAME_CallInEditor)
		{
			FunctionDesc->Meta.Add(Spec.Name, TEXT("true"));
		}
		else if (
			   Spec.Name == PP_NAME_Category
			|| Spec.Name == PP_NAME_Keywords
			|| Spec.Name == PP_NAME_ToolTip
			|| Spec.Name == PP_NAME_DisplayName
			|| Spec.Name == PP_NAME_BlueprintProtected
		){
			FunctionDesc->Meta.Add(Spec.Name, Spec.Value);
		}
		else if (Spec.Name == PP_NAME_Meta)
		{
			for (auto& Elem : Spec.List)
			{
				if (!Elem.Name.IsNone())
					FunctionDesc->Meta.Add(Elem.Name, Elem.Value);
			}
		}
		else if (Spec.Name == PP_NAME_ForcedAssets)
		{
			FString AssetsMeta;
			for (auto& Elem : Spec.List)
			{
				if (AssetsMeta.Len() != 0)
					AssetsMeta += TEXT(";");
				AssetsMeta += FString::Printf(TEXT("%s=%s"), *Elem.Name.ToString(), *Elem.Value);
			}

			FunctionDesc->Meta.Add(PP_NAME_ForcedAssets, AssetsMeta);
		}
		else
		{
			MacroError(File, Macro, FString::Printf(TEXT("Unknown function specifier %s on method %s::%s."),
				*Spec.Name.ToString(), *ClassDesc->ClassName, *FunctionDesc->ScriptFunctionName));
			bHasError = true;
		}
	}

	bool bRemoveFunctionOutsideEditor = false;
	if (ShouldRemoveFunctionOutsideEditor.IsBound())
		bRemoveFunctionOutsideEditor = ShouldRemoveFunctionOutsideEditor.Execute(*this, ClassDesc, FunctionDesc);

	if (bRemoveFunctionOutsideEditor)
	{
		if (!FAngelscriptManager::bUseEditorScripts)
		{
			// Remove the function entirely if we aren't using editor scripts
			ClassDesc->Methods.Remove(FunctionDesc);
			ReplaceWithBlank(Chunk, Macro.MacroStartPos, Macro.FunctionEndChunkOffset);
			RemoveReplacementsInRange(Chunk, Macro.MacroStartPos, Macro.FunctionEndChunkOffset);
			return;
		}
		else
		{
			// Need to add the lines that the function is on as an editor-only block,
			// so the compiler knows to treat the code as editor-only
			AddEditorOnlyLines(File, Macro.FileLineNumber, Macro.FunctionEndLineNumber);
			FunctionDesc->Meta.Add(PP_NAME_EditorOnly, TEXT(""));
		}
	}

	// Replace the script function name if we need to
	if (FunctionDesc->ScriptFunctionName != Macro.Name)
		ReplaceInChunk(Chunk, Macro.NameStartPos, Macro.NameEndPos, FunctionDesc->ScriptFunctionName);

	// Blank out the actual macro from the code angelscript reads
	ReplaceWithBlank(Chunk, Macro.MacroStartPos, Macro.MacroEndPos);
}


FString FAngelscriptPreprocessor::ExtractArgumentList(FChunk& Chunk, FMacro& Macro, TArray<FString>& OutArgNames, TArray<FString>& OutArgTypes, bool& OutConst, bool& OutProperty)
{
	int32 Pos = Macro.NameEndPos;
	int32 Len = Chunk.Content.Len();
	int32 BracketDepth = 0;
	int32 AngleDepth = 0;

	int32 ArgStart = -1;
	int32 ArgEnd = -1;
	bool bRecordedArg = false;

	int32 EndOfPreviousArg = 0;

	OutConst = false;

	auto RecordArg = [&]()
	{
		// Read backwards from end to get name, we are currently on the ; or ( that ends the variable/function name
		int32 EndOfWord = Pos-1;
		while (EndOfWord > 0 && IsWhitespace(Chunk.Content[EndOfWord]))
			EndOfWord -= 1;
		int32 StartOfWord = EndOfWord;
		while (StartOfWord > 0 && !IsWhitespace(Chunk.Content[StartOfWord]))
			StartOfWord -= 1;

		bool bMadeArg = false;
		if (EndOfWord > StartOfWord)
		{
			FString Arg = Chunk.Content.Mid(StartOfWord + 1, EndOfWord - StartOfWord).TrimStartAndEnd();
			if (Arg.Len() > 0 && Arg[Arg.Len() - 1] != ',')
			{
				bMadeArg = true;
				OutArgNames.Add(Arg);
			}
		}

		// Read backwards further to get the argument type
		if (bMadeArg && StartOfWord > 0)
		{
			EndOfWord = StartOfWord - 1;

			int AngleBrackets = 0;
			while (StartOfWord > EndOfPreviousArg)
			{
				StartOfWord -= 1;

				TCHAR Char = Chunk.Content[StartOfWord];
				if ((Char == ',' && AngleBrackets <= 0) || Char == '(')
					break;
				else if (Char == '>')
					AngleBrackets += 1;
				else if (Char == '<')
					AngleBrackets -= 1;
			}

			if (EndOfWord > StartOfWord)
			{
				FString ArgType = Chunk.Content.Mid(StartOfWord + 1, EndOfWord - StartOfWord).TrimStartAndEnd();
				OutArgTypes.Add(ArgType.TrimStartAndEnd());
			}
			else
			{
				OutArgTypes.Add(TEXT(""));
			}
		}

		EndOfPreviousArg = EndOfWord;
	};

	for (; Pos < Len; ++Pos)
	{
		int16 Char = Chunk.Content[Pos];
		switch (Char)
		{
		case '(':
			if (BracketDepth == 0)
			{
				ArgStart = Pos;
				EndOfPreviousArg = Pos;
			}
			BracketDepth += 1;
			AngleDepth = 0;
		break;
		case ',':
			if (BracketDepth == 1 && AngleDepth <= 0)
			{
				if (!bRecordedArg)
					RecordArg();
				bRecordedArg = false;
			}
		break;
		case '=':
			if (BracketDepth == 1)
			{
				if (!bRecordedArg)
					RecordArg();
				bRecordedArg = true;
			}
		break;
		case '<':
			if (BracketDepth == 1)
			{
				AngleDepth += 1;
			}
		break;
		case '>':
			if (BracketDepth == 1)
			{
				AngleDepth -= 1;
			}
		break;
		case ')':
			BracketDepth -= 1;
			if (BracketDepth == 0)
			{
				if(Pos > ArgStart + 1 && !bRecordedArg)
					RecordArg();
				ArgEnd = Pos;

				// Check if there is a 'const' token behind the brackets
				for (++Pos; Pos < Len; ++Pos)
				{
					int16 SuffixChar = Chunk.Content[Pos];
					if (SuffixChar == '\t' || SuffixChar == ' ')
						continue;

					if (SuffixChar == 'c')
					{
						if ((Len - Pos) >= 5
							&& FCString::Strncmp(&Chunk.Content[Pos], TEXT("const"), 5) == 0)
						{
							OutConst = true;
							Pos += 5 - 1;
						}
					}
					else if (SuffixChar == 'p')
					{
						if ((Len - Pos) >= 8 && FCString::Strncmp(&Chunk.Content[Pos], TEXT("property"), 8) == 0)
						{
							OutProperty = true;
							Pos += 8 - 1;
						}
					}
					else
					{
						break;
					}

				}

				// Skip to the end
				Pos = Len;
			}
		break;
		}
	}

	check(ArgStart != -1);
	if (ArgEnd == -1)
		return TEXT("invalid");

	FString ArgList = Chunk.Content.Mid(ArgStart+1, ArgEnd - ArgStart - 1);
	for (TCHAR& Char : ArgList)
	{
		if (Char == '\n' || Char == '\r')
			Char = ' ';
	}
	return ArgList;
}

FString FAngelscriptPreprocessor::ExtractReturnType(FChunk& Chunk, FMacro& Macro, bool& OutIsConst, FString& OutAccessSpecifier)
{
	// Read backwards from name to get return type
	int32 EndOfWord = Macro.NameStartPos-1;
	while (EndOfWord >= 0 && IsWhitespace(Chunk.Content[EndOfWord]))
		EndOfWord -= 1;
	int32 StartOfWord = EndOfWord;
	while (StartOfWord >= 0 && !IsWhitespace(Chunk.Content[StartOfWord]))
		StartOfWord -= 1;

	// Check if there are any qualifiers in front
	int32 EndOfQualifier = StartOfWord - 1;
	while (true)
	{
		while (EndOfQualifier >= 0 && IsWhitespace(Chunk.Content[EndOfQualifier]))
			EndOfQualifier -= 1;

		int32 StartOfQualifier = EndOfQualifier;
		while (StartOfQualifier >= 0 && !IsWhitespace(Chunk.Content[StartOfQualifier]))
			StartOfQualifier -= 1;

		if (StartOfQualifier != EndOfQualifier)
		{
			FString Qualifier = Chunk.Content.Mid(StartOfQualifier+1, EndOfQualifier - StartOfQualifier);
			if (Qualifier == TEXTVIEW("const"))
			{
				OutIsConst = true;
				EndOfQualifier = StartOfQualifier;
				continue;
			}
			else if (Qualifier == TEXTVIEW("private") || Qualifier == TEXTVIEW("protected"))
			{
				OutAccessSpecifier = Qualifier;
				break;
			}
			else
			{
				break;
			}
		}
		else
		{
			break;
		}
	}

	FString ReturnType = Chunk.Content.Mid(StartOfWord+1, EndOfWord - StartOfWord + 1);
	for (TCHAR& Char : ReturnType)
	{
		if (Char == '\n' || Char == '\r')
			Char = ' ';
	}

	ReturnType.TrimStartAndEndInline();
	return ReturnType;
}

void FAngelscriptPreprocessor::GenerateBlueprintEventWrapper(FFile& File, FChunk& Chunk, FMacro& Macro, TSharedPtr<FAngelscriptFunctionDesc> FunctionDesc)
{
	TArray<FString> ArgumentNames;
	TArray<FString> ArgumentTypes;
	bool bConstMethod = false;
	bool bPropertyMethod = false;
	FString Arguments = ExtractArgumentList(Chunk, Macro, ArgumentNames, ArgumentTypes, bConstMethod, bPropertyMethod);

	FString AccessSpecifier;
	bool bReturnIsConst = false;
	FString ReturnType = ExtractReturnType(Chunk, Macro, bReturnIsConst, AccessSpecifier);

	// Remove any visibility specifiers we may have in front of the return type
	FString ReturnTypeWithVisibility = ReturnType;
	if (bReturnIsConst)
		ReturnTypeWithVisibility = TEXT("const ") + ReturnTypeWithVisibility;
	if (!AccessSpecifier.IsEmpty())
		ReturnTypeWithVisibility = AccessSpecifier + TEXT(" ") + ReturnTypeWithVisibility;

	bool bHaveReturn = ReturnType != TEXT("void");
	check(!FunctionDesc->bIsStatic);

	// Generate the actual function code for the wrapper
	FString Code;
	Code += FString::Printf(TEXT("%s %s(%s) %sfinal%s {"),
		*ReturnTypeWithVisibility, *FunctionDesc->FunctionName, *Arguments,
		bConstMethod ? TEXT("const ") : TEXT(""),
		bPropertyMethod ? TEXT(" property") : TEXT(""));

	for (int32 ArgIndex = 0, ArgCount = ArgumentNames.Num(); ArgIndex < ArgCount; ++ArgIndex)
	{
		const FString& Arg = ArgumentNames[ArgIndex];
		const bool bIsRefArgument = ArgumentTypes[ArgIndex].Contains(TEXT("&"));
		if (bIsRefArgument)
		{
			const bool bIsConstRefArgument = ArgumentTypes[ArgIndex].Contains(TEXT("const "));
			if (!bIsConstRefArgument)
			{
				Code += FString::Printf(TEXT(" __Evt_PushArgumentRef%s(%s);"),
					*GetPushArgumentSuffix(ArgumentTypes[ArgIndex]),
					*Arg);
				continue;
			}
		}

		Code += FString::Printf(TEXT(" __Evt_PushArgument%s(%s);"),
			*GetPushArgumentSuffix(ArgumentTypes[ArgIndex]),
			*Arg);
	}

	if (bHaveReturn)
	{
		Code += FString::Printf(TEXT(" %s __ReturnValue%s; __Evt_PushArgumentRef%s(__ReturnValue);"),
			*ReturnType, *GetReturnInit(ReturnType), *GetPushArgumentSuffix(ReturnType));
	}

	Code += FString::Printf(TEXT(" __Evt_Execute(this, %s);"), *GenerateStaticName(File, FunctionDesc->FunctionName));

	if (bHaveReturn)
		Code += TEXT(" return __ReturnValue;");
	Code += TEXT("}");

	// Replace the macro in the chunk with the code
	ReplaceInChunk(Chunk, Macro.MacroStartPos, Macro.MacroEndPos, Code);
}

FString FAngelscriptPreprocessor::MakeIdentifier(const FString& Str)
{
	FString Result;
	Result.Reserve(Str.Len());

	for (int16 Char : Str)
	{
		if (FAngelscriptManager::IsValidIdentifierCharacter(Char))
		{
			Result += (TCHAR)Char;
		}
		else
		{
			Result += (TCHAR)'_';
		}
	}

	return Result;
}

FString FAngelscriptPreprocessor::GenerateStaticName(FFile& File, const FString& InName)
{
	FName Name = FName(*InName);
	int32* FoundIndex = FAngelscriptManager::StaticNamesByIndex.Find(Name);
	if (FoundIndex != nullptr)
		return FString::Printf(TEXT("__STATIC_NAME(%d)"), *FoundIndex);

	int32 Index = FAngelscriptManager::StaticNames.Emplace(Name);
	FAngelscriptManager::StaticNamesByIndex.Add(Name, Index);
	return FString::Printf(TEXT("__STATIC_NAME(%d)"), Index);
}

FString FAngelscriptPreprocessor::GenerateFormatString(FFile& File, const FString& FormatStr)
{
	FString Result;
	Result.Reserve(FormatStr.Len() + 128);
	Result.Append(TEXT("(FString()"));

	int32 StartPosition = 0;
	int32 Parts = 0;
	int32 Length = FormatStr.Len();
	bool bInExpression = false;

	for (int32 CurPosition = 0; CurPosition < Length; ++CurPosition)
	{
		int16 Char = FormatStr[CurPosition];
		switch (Char)
		{
		case '{':
		{
			if (!bInExpression)
			{
				if (CurPosition > StartPosition)
				{
					Result.Append(TEXT(".Append(\""));
					Result.Append(FormatStr.Mid(StartPosition, CurPosition - StartPosition));
					Result.Append(TEXT("\")"));
					Parts += 1;
				}

				if (CurPosition + 1 < Length && FormatStr[CurPosition + 1] == '{')
				{
					// Escaped brace
					Result.Append(TEXT(".AppendChar('{')"));
					CurPosition += 1;
					StartPosition = CurPosition + 1;
				}
				else
				{

					StartPosition = CurPosition + 1;
					bInExpression = true;
				}
			}
		}
		break;
		case '}':
			if (bInExpression)
			{
				if (CurPosition > StartPosition)
				{
					Result.Append(TEXT(".Append("));
					FString Expression = FormatStr.Mid(StartPosition, CurPosition - StartPosition);
					Result.Append(ParseFormatExpression(File, Expression));
					Result.AppendChar(')');
					Parts += 1;
				}

				StartPosition = CurPosition + 1;
				bInExpression = false;
			}
			else
			{
				if (CurPosition + 1 < Length && FormatStr[CurPosition + 1] == '}')
				{
					if (CurPosition > StartPosition)
					{
						Result.Append(TEXT(".Append(\""));
						Result.Append(FormatStr.Mid(StartPosition, CurPosition - StartPosition));
						Result.Append(TEXT("\")"));
						Parts += 1;
					}

					// Escaped brace
					Result.Append(TEXT(".AppendChar('}')"));

					CurPosition += 1;
					StartPosition = CurPosition + 1;
				}
			}
		break;
		}
	}

	if (!bInExpression)
	{
		if (Length > StartPosition)
		{
			Result.Append(TEXT(".Append(\""));
			Result.Append(FormatStr.Mid(StartPosition, Length - StartPosition));
			Result.Append(TEXT("\")"));
			Parts += 1;
		}
	}

	Result.AppendChar(')');
	return Result;
}

FString FAngelscriptPreprocessor::ParseFormatExpression(FFile& File, const FString& FormatExpr)
{
	int32 EqualsPos = -1;
	bool bFoundFormat = false;

	int32 Pos = FormatExpr.Len() - 1;
	for (; Pos >= 0; --Pos)
	{
		int16 Char = FormatExpr[Pos];
		if (Char == ' ' || Char == '\t')
			continue;

		if (Char == '=' && !bFoundFormat)
		{
			EqualsPos = Pos;
			continue;
		}

		bFoundFormat = true;
		if (Char == ':')
		{
			// Check if the equals is before the format specifier
			FString Specifier = FormatExpr.Mid(Pos+1);
			EqualsPos = Pos-1;
			for (; EqualsPos >= 0; --EqualsPos)
			{
				int16 EqualsChar = FormatExpr[EqualsPos];
				if (EqualsChar == ' ' || EqualsChar == '\t')
					continue;

				if (EqualsChar == '=')
				{
					FString Expr = FormatExpr.Mid(0, EqualsPos);
					Expr.TrimStartAndEndInline();
					return FString::Printf(TEXT("\"%s = \"+FString::ApplyFormat((%s), \"%s\")"), *Expr, *Expr, *Specifier);
				}
				else
				{
					break;
				}
			}

			FString Expr = FormatExpr.Mid(0, Pos);
			return FString::Printf(TEXT("FString::ApplyFormat((%s), \"%s\")"), *Expr, *Specifier);
		}

		bool bValidFormatSpec = false;
		switch (Char)
		{
		case '0':
		case '1':
		case '2':
		case '3':
		case '4':
		case '5':
		case '6':
		case '7':
		case '8':
		case '9':
		case 'd':
		case 'x':
		case 'X':
		case 'b':
		case 'c':
		case 'o':
		case 'n':
		case 'e':
		case 'E':
		case 'f':
		case 'F':
		case 'g':
		case 'G':
		case '%':
		case ',':
		case '.':
		case '-':
		case '+':
		case '(':
		case '#':
			bValidFormatSpec = true;
		break;
		case '<':
		case '>':
		case '^':
		case '=':
			if (Pos > 0 && FormatExpr[Pos-1] != ':')
				--Pos;
			bValidFormatSpec = true;
		break;
		}

		if (!bValidFormatSpec)
			break;
	}

	if (EqualsPos != -1)
	{
		FString Expr = FormatExpr.Mid(0, EqualsPos);
		Expr.TrimStartAndEndInline();
		return FString::Printf(TEXT("\"%s = \"+(%s)"), *Expr, *Expr);
	}
	else
	{
		return FormatExpr;
	}
}

static FName PP_NAME_HideCategories("HideCategories");
static FName PP_NAME_ComponentWrapperClass("ComponentWrapperClass");
static FName PP_NAME_Placeable("Placeable");
static FName PP_NAME_NotPlaceable("NotPlaceable");
static FName PP_NAME_NotBlueprintable("NotBlueprintable");
static FName PP_NAME_Blueprintable("Blueprintable");
static FName PP_NAME_Abstract("Abstract");
static FName PP_NAME_Transient("Transient");
static FName PP_NAME_HideDropdown("HideDropdown");
static FName PP_NAME_Deprecated("Deprecated");
static FName PP_NAME_Config("Config");
static FName PP_NAME_DefaultConfig("DefaultConfig");
static FName PP_NAME_Interp("Interp");
static FName PP_NAME_AssetRegistrySearchable("AssetRegistrySearchable");
static FName PP_NAME_NoClear("NoClear");
static FName PP_NAME_ClassGroup("ClassGroup");
static FName PP_NAME_ClassGroupNames("ClassGroupNames");
static FName PP_NAME_DefaultToInstanced("DefaultToInstanced");
static FName PP_NAME_EditInlineNew("EditInlineNew");

void FAngelscriptPreprocessor::ProcessClassMacro(FFile& File, FChunk& Chunk, FMacro& Macro)
{
	auto ClassDesc = Chunk.ClassDesc;
	if (!ensure(ClassDesc.IsValid()))
		return;

	TArray<FString> SpecifierParseErrors;
	TArray<FSpecifier> Specs = ParseSpecifiers(Macro.Arguments, SpecifierParseErrors);
	for (FString& Error : SpecifierParseErrors)
	{
		MacroError(File, Macro, Error);
		bHasError = true;
	}

	for (auto& Spec : Specs)
	{
		if (Spec.Name == PP_NAME_NotPlaceable)
		{
			ClassDesc->bPlaceable = false;
		}
		else if (Spec.Name == PP_NAME_Placeable)
		{
			ClassDesc->bPlaceable = true;
		}
		else if (Spec.Name == PP_NAME_NotBlueprintable)
		{
			ClassDesc->Meta.Add(TEXT("NotBlueprintable"), TEXT("true"));
			ClassDesc->Meta.Add(TEXT("IsBlueprintBase"), TEXT("false"));
		}
		else if (Spec.Name == PP_NAME_Blueprintable)
		{
			ClassDesc->Meta.Add(TEXT("IsBlueprintBase"), TEXT("true"));
			ClassDesc->Meta.Add(TEXT("Blueprintable"), TEXT("true"));
		}
		else if (Spec.Name == PP_NAME_Abstract)
		{
			ClassDesc->bAbstract = true;
		}
		else if (Spec.Name == PP_NAME_Transient)
		{
			ClassDesc->bTransient = true;
		}
		else if (Spec.Name == PP_NAME_HideDropdown)
		{
			ClassDesc->bHideDropdown = true;
		}
		else if (Spec.Name == PP_NAME_DefaultToInstanced)
		{
			ClassDesc->bDefaultToInstanced = true;
		}
		else if (Spec.Name == PP_NAME_EditInlineNew)
		{
			ClassDesc->bEditInlineNew = true;
		}
		else if (Spec.Name == PP_NAME_Deprecated)
		{
			ClassDesc->bIsDeprecatedClass = true;
		}
		else if (Spec.Name == PP_NAME_Config)
		{
			ClassDesc->ConfigName = Spec.Value;
		}
		else if (Spec.Name == PP_NAME_ClassGroup)
		{
			ClassDesc->Meta.Add(PP_NAME_ClassGroupNames, Spec.Value);
		}
		else if (Spec.Name == PP_NAME_HideCategories
			|| Spec.Name == PP_NAME_DefaultConfig
			|| Spec.Name == PP_NAME_ComponentWrapperClass)
		{
			ClassDesc->Meta.Add(Spec.Name, Spec.Value);
		}
		else if (Spec.Name == PP_NAME_Meta)
		{
			for (auto& Elem : Spec.List)
			{
				if (!Elem.Name.IsNone())
					ClassDesc->Meta.Add(Elem.Name, Elem.Value);
			}
		}
		else
		{
			MacroError(File, Macro, FString::Printf(TEXT("Unknown class specifier %s on class %s."),
				*Spec.Name.ToString(), *ClassDesc->ClassName));
			bHasError = true;
		}
	}

	ReplaceWithBlankFilePos(File, Macro.MacroStartPos + Chunk.ChunkStartPos, Macro.MacroEndPos + Chunk.ChunkStartPos);
}

static FName PP_NAME_AutoCollapseCategories("AutoCollapseCategories");
static FName PP_NAME_DefaultComponent("DefaultComponent");
static FName PP_NAME_OverrideComponent("OverrideComponent");
static FName PP_NAME_BindComponent("BindComponent");
static FName PP_NAME_ShowOnActor("ShowOnActor");
static FName PP_NAME_RootComponent("RootComponent");
static FName PP_NAME_Attach("Attach");
static FName PP_NAME_AttachSocket("AttachSocket");

static FName PP_NAME_BlueprintReadWrite("BlueprintReadWrite");
static FName PP_NAME_BlueprintReadOnly("BlueprintReadOnly");
static FName PP_NAME_BlueprintHidden("BlueprintHidden");

static FName PP_NAME_NotVisible("NotVisible");
static FName PP_NAME_NotEditable("NotEditable");
static FName PP_NAME_EditConst("EditConst");
static FName PP_NAME_EditInline("EditInline");
static FName PP_NAME_EditInlineDefaults("EditInlineDefaults");
static FName PP_NAME_EditInstanceOnly("EditInstanceOnly");
static FName PP_NAME_EditDefaultsOnly("EditDefaultsOnly");
static FName PP_NAME_VisibleInstanceOnly("VisibleInstanceOnly");
static FName PP_NAME_VisibleDefaultsOnly("VisibleDefaultsOnly");
static FName PP_NAME_VisibleAnywhere("VisibleAnywhere");
static FName PP_NAME_EditAnywhere("EditAnywhere");
static FName PP_NAME_Replicated("Replicated");
static FName PP_NAME_ReplicationCondition("ReplicationCondition");
static FName PP_NAME_ReplicationPushModel("ReplicationPushModel");
static FName PP_NAME_ReplicatedUsing("ReplicatedUsing");
static FName PP_NAME_EditFixedSize("EditFixedSize");
static FName PP_NAME_NotReplicated("NotReplicated");
static FName PP_NAME_Instanced("Instanced");
static FName PP_NAME_SkipSerialization("SkipSerialization");
static FName PP_NAME_SaveGame("SaveGame");

static FName PP_NAME_AdvancedDisplay("AdvancedDisplay");
static FName PP_NAME_ExposeOnSpawn("ExposeOnSpawn");
static FName PP_NAME_BindWidget("BindWidget");
static FName PP_NAME_BindWidgetAnim("BindWidgetAnim");
static FName PP_NAME_BindWidgetOptional("BindWidgetOptional");

void FAngelscriptPreprocessor::ProcessPropertyMacro(FFile& File, FChunk& Chunk, FMacro& Macro)
{
	// Create the property descriptor
	auto PropDesc = MakeShared<FAngelscriptPropertyDesc>();
	PropDesc->LineNumber = Macro.FileLineNumber;

	auto ClassDesc = Chunk.ClassDesc;
	if (!ClassDesc.IsValid())
		return;

	ClassDesc->Properties.Add(PropDesc);

	PropDesc->PropertyName = Macro.Name;
	PropDesc->LiteralType = Macro.SubjectType;
	PropDesc->bEditConst = false;

	// Default Editable level for properties
	auto EditSpecifier = ClassDesc->bIsStruct ? DefaultPropertyEditSpecifierForStructs : DefaultPropertyEditSpecifier;
	switch (EditSpecifier)
	{
	case EAngelscriptPropertyEditSpecifier::EditAnywhere:
		PropDesc->bEditableOnDefaults = true;
		PropDesc->bEditableOnInstance = true;
	break;
	case EAngelscriptPropertyEditSpecifier::EditInstanceOnly:
		PropDesc->bEditableOnDefaults = false;
		PropDesc->bEditableOnInstance = true;
	break;
	case EAngelscriptPropertyEditSpecifier::EditDefaultsOnly:
		PropDesc->bEditableOnDefaults = true;
		PropDesc->bEditableOnInstance = false;
	break;
	case EAngelscriptPropertyEditSpecifier::NotEditable:
		PropDesc->bEditableOnDefaults = false;
		PropDesc->bEditableOnInstance = false;
	break;
	}

	// Default blueprint access level for properties
	switch (DefaultPropertyBlueprintSpecifier)
	{
	case EAngelscriptPropertyBlueprintSpecifier::BlueprintReadWrite:
		PropDesc->bBlueprintReadable = true;
		PropDesc->bBlueprintWritable = true;
	break;
	case EAngelscriptPropertyBlueprintSpecifier::BlueprintReadOnly:
		PropDesc->bBlueprintReadable = true;
		PropDesc->bBlueprintWritable = false;
	break;
	case EAngelscriptPropertyBlueprintSpecifier::BlueprintHidden:
		PropDesc->bBlueprintReadable = false;
		PropDesc->bBlueprintWritable = false;
	break;
	}

	if (Macro.bEditorOnly)
		PropDesc->Meta.Add(PP_NAME_EditorOnly, TEXT(""));

	bool bHadShowOnActor = false;
	bool bHadRootComponent = false;
	bool bHadAttachment = false;
	bool bIsDefaultComponent = false;
	bool bIsOverrideComponent = false;
	bool bIsBindComponent = false;

#if WITH_EDITOR
	if(Macro.Comment.Len() != 0)
		PropDesc->Meta.Add(PP_NAME_ToolTip, FormatCommentForToolTip(Macro.Comment));
#endif

	OnModifyPropertyDefaults.Broadcast(*this, ClassDesc, PropDesc);

	TArray<FString> SpecifierParseErrors;
	TArray<FSpecifier> Specs = ParseSpecifiers(Macro.Arguments, SpecifierParseErrors);
	for (FString& Error : SpecifierParseErrors)
	{
		MacroError(File, Macro, Error);
		bHasError = true;
	}

	for (auto& Spec : Specs)
	{
		if (Spec.Name == PP_NAME_BlueprintReadWrite)
		{
			PropDesc->bBlueprintWritable = true;
			PropDesc->bBlueprintReadable = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintReadOnly)
		{
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintHidden)
		{
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = false;
		}
		else if (Spec.Name == PP_NAME_EditInstanceOnly)
		{
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = true;
		}
		else if (Spec.Name == PP_NAME_EditDefaultsOnly)
		{
			PropDesc->bEditableOnDefaults = true;
			PropDesc->bEditableOnInstance = false;
		}
		else if (Spec.Name == PP_NAME_EditAnywhere)
		{
			PropDesc->bEditableOnDefaults = true;
			PropDesc->bEditableOnInstance = true;
		}
		else if (Spec.Name == PP_NAME_NotVisible)
		{
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bEditConst = false;
		}
		else if (Spec.Name == PP_NAME_NotEditable)
		{
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bEditConst = false;
		}
		else if (Spec.Name == PP_NAME_EditConst)
		{
			PropDesc->bEditConst = true;
		}
		else if (Spec.Name == PP_NAME_VisibleAnywhere)
		{
			PropDesc->bEditConst = true;
			PropDesc->bEditableOnDefaults = true;
			PropDesc->bEditableOnInstance = true;
		}
		else if (Spec.Name == PP_NAME_VisibleInstanceOnly)
		{
			PropDesc->bEditConst = true;
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = true;
		}
		else if (Spec.Name == PP_NAME_VisibleDefaultsOnly)
		{
			PropDesc->bEditConst = true;
			PropDesc->bEditableOnDefaults = true;
			PropDesc->bEditableOnInstance = false;
		}
		else if (Spec.Name == PP_NAME_BindWidget)
		{
			PropDesc->Meta.Add(PP_NAME_BindWidget, TEXT(""));
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = true;
			PropDesc->bTransient = true;
		}
		else if (Spec.Name == PP_NAME_BindWidgetAnim)
		{
			PropDesc->Meta.Add(PP_NAME_BindWidgetAnim, TEXT(""));
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = true;
			PropDesc->bTransient = true;
		}
		else if (Spec.Name == PP_NAME_BindWidgetOptional)
		{
			PropDesc->Meta.Add(PP_NAME_BindWidgetOptional, TEXT(""));
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = true;
			PropDesc->bTransient = true;
		}
#if !WITH_ANGELSCRIPT_HAZE
		else if (Spec.Name == PP_NAME_Replicated)
		{
			PropDesc->bReplicated = true;
		}
		else if (Spec.Name == PP_NAME_ReplicationCondition)
		{
			if (Spec.Value == TEXT("None"))
				PropDesc->ReplicationCondition = COND_None;
			else if (Spec.Value == TEXT("InitialOnly"))
				PropDesc->ReplicationCondition = COND_InitialOnly;
			else if (Spec.Value == TEXT("OwnerOnly"))
				PropDesc->ReplicationCondition = COND_OwnerOnly;
			else if (Spec.Value == TEXT("SkipOwner"))
				PropDesc->ReplicationCondition = COND_SkipOwner;
			else if (Spec.Value == TEXT("SimulatedOnly"))
				PropDesc->ReplicationCondition = COND_SimulatedOnly;
			else if (Spec.Value == TEXT("AutonomousOnly"))
				PropDesc->ReplicationCondition = COND_AutonomousOnly;
			else if (Spec.Value == TEXT("SimulatedOrPhysics"))
				PropDesc->ReplicationCondition = COND_SimulatedOrPhysics;
			else if (Spec.Value == TEXT("InitialOrOwner"))
				PropDesc->ReplicationCondition = COND_InitialOrOwner;
			else if (Spec.Value == TEXT("Custom"))
				PropDesc->ReplicationCondition = COND_Custom;
			else if (Spec.Value == TEXT("ReplayOrOwner"))
				PropDesc->ReplicationCondition = COND_ReplayOrOwner;
			else if (Spec.Value == TEXT("ReplayOnly"))
				PropDesc->ReplicationCondition = COND_ReplayOnly;
			else if (Spec.Value == TEXT("SimulatedOnlyNoReplay"))
				PropDesc->ReplicationCondition = COND_SimulatedOnlyNoReplay;
			else if (Spec.Value == TEXT("SimulatedOrPhysicsNoReplay"))
				PropDesc->ReplicationCondition = COND_SimulatedOrPhysicsNoReplay;
			else if (Spec.Value == TEXT("SkipReplay"))
				PropDesc->ReplicationCondition = COND_SkipReplay;
			else
			{
				MacroError(File, Macro, FString::Printf(TEXT("Unknown ReplicationCondition %s on property %s::%s."),
					*Spec.Value, *ClassDesc->ClassName, *PropDesc->PropertyName));
				bHasError = true;
			}
		}
		else if (Spec.Name == PP_NAME_ReplicationPushModel)
		{
			PropDesc->Meta.Add(PP_NAME_ReplicationPushModel);
		}
		else if (Spec.Name == PP_NAME_ReplicatedUsing)
		{
			if (!Spec.Value.IsEmpty())
			{
				PropDesc->bReplicated = true;
				PropDesc->bRepNotify = true;
				PropDesc->Meta.Add(PP_NAME_ReplicatedUsing, Spec.Value);
			}
			else
			{
				MacroError(File, Macro, FString::Printf(TEXT("No function specified for ReplicatedUsing on property %s::%s."),
					*ClassDesc->ClassName, *PropDesc->PropertyName));
				bHasError = true;
			}
		}
		else if (Spec.Name == PP_NAME_NotReplicated)
		{
			if (Chunk.Type != EChunkType::Struct)
			{
				MacroError(File, Macro, FString::Printf(TEXT("The %s specifier is only allowed structs."), *PP_NAME_NotReplicated.ToString()));
				bHasError = true;
			}
			else
			{
				PropDesc->bSkipReplication = true;
			}
		}
#endif
		else if (Spec.Name == PP_NAME_SkipSerialization)
		{
			PropDesc->bSkipSerialization = true;
		}
		else if (Spec.Name == PP_NAME_SaveGame)
		{
			PropDesc->bSaveGame = true;
		}
		else if (Spec.Name == PP_NAME_AdvancedDisplay)
		{
			PropDesc->bAdvancedDisplay = true;
		}
		else if (Spec.Name == PP_NAME_Transient)
		{
			PropDesc->bTransient = true;
		}
		else if (Spec.Name == PP_NAME_Config)
		{
			PropDesc->bConfig = true;
		}
		else if (Spec.Name == PP_NAME_Interp)
		{
			PropDesc->bInterp = true;
		}
		else if (Spec.Name == PP_NAME_AssetRegistrySearchable)
		{
			PropDesc->bAssetRegistrySearchable = true;
		}
		else if (Spec.Name == PP_NAME_NoClear)
		{
			PropDesc->bNoClear = true;
		}
		else if (Spec.Name == PP_NAME_OverrideComponent)
		{
			PropDesc->bEditConst = false;
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = false;
			PropDesc->bInstancedReference = true;
			PropDesc->Meta.Add(Spec.Name, Spec.Value);
			bIsOverrideComponent = true;
		}
		else if (Spec.Name == PP_NAME_DefaultComponent)
		{
			if (!bHadShowOnActor)
			{
				if (FAngelscriptManager::ConfigSettings->bDefaultComponentsEditable)
				{
					PropDesc->bEditableOnDefaults = true;
					PropDesc->bEditConst = false;
				}
				else
				{
					PropDesc->bEditableOnDefaults = false;
					PropDesc->bEditConst = true;
				}
				PropDesc->bEditableOnInstance = false;
			}

			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = true;
			PropDesc->bInstancedReference = true;
			bIsDefaultComponent = true;

			PropDesc->Meta.Add(Spec.Name, TEXT("True"));
		}
		else if (Spec.Name == PP_NAME_BindComponent)
		{
			PropDesc->bEditableOnDefaults = false;
			PropDesc->bEditableOnInstance = false;
			PropDesc->bBlueprintWritable = false;
			PropDesc->bBlueprintReadable = false;
			PropDesc->bTransient = true;
			PropDesc->bInstancedReference = true;

			bIsBindComponent = true;
			PropDesc->Meta.Add(Spec.Name, Spec.Value);
			
			// Append a suffix to the Unreal FProperty, to prevent name collision with the property we want to bind to
			PropDesc->UnrealPropertyNameOverride = PropDesc->PropertyName + TEXT("__BindComponent");
		}
		else if (Spec.Name == PP_NAME_ShowOnActor)
		{
			bHadShowOnActor = true;
			PropDesc->bEditConst = false;
			PropDesc->bEditableOnDefaults = true;
			PropDesc->bEditableOnInstance = true;
			PropDesc->Meta.Add(PP_NAME_EditInlineDefaults, TEXT("true"));
			PropDesc->Meta.Add(PP_NAME_EditInline, TEXT("true"));
		}
		else if (
			   Spec.Name == PP_NAME_Category
			|| Spec.Name == PP_NAME_Keywords
			|| Spec.Name == PP_NAME_ToolTip
			|| Spec.Name == PP_NAME_DisplayName
			|| Spec.Name == PP_NAME_EditInline
			|| Spec.Name == PP_NAME_ExposeOnSpawn
			|| Spec.Name == PP_NAME_EditFixedSize
			|| Spec.Name == PP_NAME_BlueprintProtected
		){
			PropDesc->Meta.Add(Spec.Name, Spec.Value);
		}
		else if (Spec.Name == PP_NAME_RootComponent)
		{
			bHadRootComponent = true;
			PropDesc->Meta.Add(Spec.Name, Spec.Value);
		}
		else if (
			  Spec.Name == PP_NAME_Attach
			|| Spec.Name == PP_NAME_AttachSocket
		){
			bHadAttachment = true;
			PropDesc->Meta.Add(Spec.Name, Spec.Value);
		}
		else if (Spec.Name == PP_NAME_Meta)
		{
			for (auto& Elem : Spec.List)
			{
				if (!Elem.Name.IsNone())
					PropDesc->Meta.Add(Elem.Name, Elem.Value);
			}
		}
		else if (Spec.Name == PP_NAME_Instanced)
		{
			PropDesc->bPersistentInstance = true;
		}
		else if (Spec.Name == PP_NAME_BlueprintSetter)
		{
			if (!Spec.Value.IsEmpty())
			{
				PropDesc->Meta.Add(PP_NAME_BlueprintSetter, Spec.Value);
			}
			else
			{
				MacroError(File, Macro,	FString::Printf(TEXT("No function specified for BlueprintSetter on property %s::%s."),
						*ClassDesc->ClassName, *PropDesc->PropertyName));
				bHasError = true;
			}
		}
		else if (Spec.Name == PP_NAME_BlueprintGetter)
		{
			if (!Spec.Value.IsEmpty())
			{
				PropDesc->Meta.Add(PP_NAME_BlueprintGetter, Spec.Value);
			}
			else
			{
				MacroError(File, Macro,	FString::Printf(TEXT("No function specified for BlueprintGetter on property %s::%s."),
						*ClassDesc->ClassName, *PropDesc->PropertyName));
				bHasError = true;
			}
		}
		else
		{
			MacroError(File, Macro, FString::Printf(TEXT("Unknown property specifier %s on property %s::%s."),
				*Spec.Name.ToString(), *ClassDesc->ClassName, *PropDesc->PropertyName));
			bHasError = true;
		}
	}

	// Blank out the actual macro from the code angelscript reads
	ReplaceWithBlank(Chunk, Macro.MacroStartPos, Macro.MacroEndPos);

	// Error handling based on property combinations
	if (bHadShowOnActor && !PropDesc->bInstancedReference && !PropDesc->bPersistentInstance)
	{
		MacroError(File, Macro, TEXT("ShowOnActor can only be used on default components in actors"));
		bHasError = true;
	}

	if (!bIsDefaultComponent)
	{
		if (bHadAttachment)
		{
			MacroError(File, Macro, TEXT("Attachments can only be specified on DefaultComponents"));
			bHasError = true;
		}
	
		if (bHadRootComponent)
		{
			MacroError(File, Macro, TEXT("RootComponent can only be specified on DefaultComponents"));
			bHasError = true;
		}
	}
	else
	{
		if (bIsOverrideComponent)
		{
			MacroError(File, Macro, TEXT("OverrideComponent and DefaultComponent should not be used simultaneously"));
			bHasError = true;
		}

		if (bIsBindComponent)
		{
			MacroError(File, Macro, TEXT("DefaultComponent and BindComponent should not be used simultaneously"));
			bHasError = true;
		}
	}

	if (bIsBindComponent)
	{
		if (bIsOverrideComponent)
		{
			MacroError(File, Macro, TEXT("OverrideComponent and BindComponent should not be used simultaneously"));
			bHasError = true;
		}

		if (PropDesc->bBlueprintReadable || PropDesc->bBlueprintWritable || PropDesc->bEditableOnDefaults || PropDesc->bEditableOnInstance)
		{
			MacroError(File, Macro, TEXT("BindComponent should not be exposed to blueprint at all. Remove any BlueprintRead/Write and Editable specifiers."));
			bHasError = true;
		}
	}

	if (bIsOverrideComponent || bIsBindComponent || bIsDefaultComponent)
	{
		if (ClassDesc->CodeSuperClass != nullptr && !ClassDesc->CodeSuperClass->IsChildOf(AActor::StaticClass()))
		{
			if (bIsDefaultComponent)
				MacroError(File, Macro, TEXT("Only Actor classes can have DefaultComponents"));
			else if (bIsBindComponent)
				MacroError(File, Macro, TEXT("Only Actor classes can use BindComponent"));
			else
				MacroError(File, Macro, TEXT("Only Actor classes can use OverrideComponent"));
			bHasError = true;
		}
	}
}

void FAngelscriptPreprocessor::DetectEnum(FFile& File, FChunk& Chunk)
{
	static const FRegexPattern EnumPattern(TEXT("(enum)\\s+([A-Za-z0-9_]+)"));

	FRegexMatcher MatchEnum(EnumPattern, Chunk.Content);
	if (!ensureMsgf(MatchEnum.FindNext(), TEXT("Enum code chunk did not include a valid enum declaration???")))
		return;


	TSharedRef<FAngelscriptEnumDesc> EnumDesc = MakeShared<FAngelscriptEnumDesc>();
	EnumDesc->LineNumber = Chunk.FileLineNumber;
	EnumDesc->EnumName = MatchEnum.GetCaptureGroup(2);
	EnumDesc->Documentation = FormatCommentForToolTip(Chunk.Comment);

	for (FMacro& Macro : Chunk.Macros)
	{
		if (Macro.Type == EMacroType::EnumValue)
		{
			auto MetaKey = TPair<FName,int32>(PP_NAME_ToolTip, Macro.SubjectIndex);
			if (!EnumDesc->Meta.Contains(MetaKey))
				EnumDesc->Meta.Add(MetaKey, FormatCommentForToolTip(Macro.Comment));
		}
		else if (Macro.Type == EMacroType::Enum)
		{
			TArray<FString> SpecifierParseErrors;
			TArray<FSpecifier> Specs = ParseSpecifiers(Macro.Arguments, SpecifierParseErrors);
			for (FString& Error : SpecifierParseErrors)
			{
				MacroError(File, Macro, Error);
				bHasError = true;
			}

			for (auto& Spec : Specs)
			{
				if (
					   Spec.Name == PP_NAME_Category
					|| Spec.Name == PP_NAME_Keywords
					|| Spec.Name == PP_NAME_ToolTip
					|| Spec.Name == PP_NAME_DisplayName
				){
					EnumDesc->Meta.Add(TPair<FName,int32>(Spec.Name, INDEX_NONE), Spec.Value);
				}
				else if (Spec.Name == PP_NAME_Meta)
				{
					for (auto& Elem : Spec.List)
					{
						if (!Elem.Name.IsNone())
							EnumDesc->Meta.Add(TPair<FName,int32>(Elem.Name, INDEX_NONE), Elem.Value);
					}
				}
				else
				{
					MacroError(File, Macro, FString::Printf(TEXT("Unknown enum specifier %s on enum %s."),
						*Spec.Name.ToString(), *EnumDesc->EnumName));
					bHasError = true;
				}
			}

			// Blank out the actual macro from the code angelscript reads
			ReplaceWithBlankFilePos(File, Macro.MacroStartPos + Chunk.ChunkStartPos, Macro.MacroEndPos + Chunk.ChunkStartPos);
		}
		else if (Macro.Type == EMacroType::EnumMeta)
		{
			TArray<FString> SpecifierParseErrors;
			TArray<FSpecifier> Specs = ParseSpecifiers(Macro.Arguments, SpecifierParseErrors);
			for (FString& Error : SpecifierParseErrors)
			{
				MacroError(File, Macro, Error);
				bHasError = true;
			}

			for (auto& Spec : Specs)
				EnumDesc->Meta.Add(TPair<FName,int32>(Spec.Name, Macro.SubjectIndex), Spec.Value);

			// Blank out the actual macro from the code angelscript reads
			ReplaceWithBlank(Chunk, Macro.MacroStartPos, Macro.MacroEndPos);
		}
	}

	File.Module->Enums.Add(EnumDesc);
}


TSharedPtr<FAngelscriptClassDesc> FAngelscriptPreprocessor::GetClassDescFor(const FString& ClassName)
{
                                                                                                   
	const auto* Unit = FAngelscriptManager::Get().CurrentScriptUnit;
	if (Unit && !ClassName.Contains(TEXT("::")))
	{
		if (auto* Own = PreprocessingClasses.Find(Unit->Id + TEXT("::") + ClassName)) return *Own;
		TSharedPtr<FAngelscriptClassDesc> Dependency;
		for (const FString& Id : Unit->Dependencies)
			if (auto* Found = PreprocessingClasses.Find(Id + TEXT("::") + ClassName))
			{
				if (Dependency.IsValid() && Dependency != *Found) return nullptr;
				Dependency = *Found;
			}
		if (Dependency.IsValid()) return Dependency;
	}
                                                                                                 
	auto* Found = PreprocessingClasses.Find(ClassName);
	if (Found != nullptr)
		return *Found;
	else
		return nullptr;
}

FAngelscriptPreprocessor::FFile* FAngelscriptPreprocessor::GetFileForClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc)
{
	for (auto& OtherFile : Files)
	{
		for (auto& OtherChunk : OtherFile.ChunkedCode)
		{
			if (OtherChunk.ClassDesc == ClassDesc)
			{
				return &OtherFile;
			}
		}
	}

	return nullptr;
}


static FName CP_NAME_CannotDeriveAngelscript("CannotDeriveAngelscript");
void FAngelscriptPreprocessor::ResolveSuperClass(TSharedPtr<FAngelscriptClassDesc> ClassDesc, bool bShowError)
{
                                                                                                
	TSharedPtr<FAngelscriptModuleDesc> Owner;
	for (const auto& File : Files)
		if (File.Module->Classes.Contains(ClassDesc.ToSharedRef())) { Owner = File.Module; break; }
	if (!Owner.IsValid())
		for (const auto& Active : FAngelscriptManager::Get().GetActiveModules())
			if (Active->Classes.Contains(ClassDesc.ToSharedRef())) { Owner = Active; break; }
	TGuardValue<const FAngelscriptScriptUnit*> UnitContext(FAngelscriptManager::Get().CurrentScriptUnit,
		Owner.IsValid() ? &Owner->ScriptUnit : FAngelscriptManager::Get().CurrentScriptUnit);
                                                                                              

	if (ClassDesc->CodeSuperClass != nullptr)
		return; // Already resolved earlier

	ResolvingClasses.Add(ClassDesc);
	ClassDesc->bSuperIsCodeClass = false;

	auto SuperType = FAngelscriptType::GetByAngelscriptTypeName(ClassDesc->SuperClass);
	if (SuperType.IsValid())
	{
		UClass* Class = SuperType->GetClass(FAngelscriptTypeUsage::DefaultUsage);
		if (Class != nullptr)
		{
			ClassDesc->bSuperIsCodeClass = true;
			ClassDesc->CodeSuperClass = Class;

#if WITH_EDITOR
			// Check to make sure we're actually allowed to derive from this code class
			if (Class->HasMetaData(CP_NAME_CannotDeriveAngelscript))
			{
				auto* File = GetFileForClass(ClassDesc);
				if (File != nullptr)
				{
					LineError(*File, ClassDesc->LineNumber, FString::Printf(TEXT("Class %s cannot inherit from C++ class %s which specifies CannotDeriveAngelscript meta"),
						*ClassDesc->ClassName, *ClassDesc->SuperClass));
				}
				bHasError = true;
			}
#endif
		}
	}

                                                                                               
	if (!ClassDesc->bSuperIsCodeClass)
	{
		FAngelscriptTypeGenerationContext Context;
		Context.Point = EAngelscriptTypeGenerationPoint::ResolveSuperClass;
		Context.Preprocessor = this;
		Context.Class = ClassDesc.Get();
		FString Error;
		if (!DispatchAngelscriptTypeGeneration(Context, Error))
		{
			if (auto* File = GetFileForClass(ClassDesc))
				LineError(*File, ClassDesc->LineNumber, Error);
			bHasError = true;
			ResolvingClasses.Remove(ClassDesc);
			return;
		}
	}
                                                                                             

	if (!ClassDesc->bSuperIsCodeClass)
	{
		TSharedPtr<FAngelscriptClassDesc> SuperClassDesc;

		SuperClassDesc = GetClassDescFor(ClassDesc->SuperClass);
		if (!SuperClassDesc.IsValid())
		{
			SuperClassDesc = FAngelscriptManager::Get().GetClass(ClassDesc->SuperClass);
		}
		else
		{
			if (ResolvingClasses.Contains(SuperClassDesc))
				SuperClassDesc.Reset();
			else
				ResolveSuperClass(SuperClassDesc, false);
		}

		if (SuperClassDesc.IsValid())
		{
			// Find the supermost class to find the correct code class
			TSharedPtr<FAngelscriptClassDesc> Supermost = SuperClassDesc;
			while (!Supermost->bSuperIsCodeClass)
			{
				TSharedPtr<FAngelscriptClassDesc> CheckSuper = GetClassDescFor(Supermost->SuperClass);
				if (!CheckSuper.IsValid())
					CheckSuper = FAngelscriptManager::Get().GetClass(Supermost->SuperClass);

				if (!CheckSuper.IsValid())
					break;
				else if (!ResolvingClasses.Contains(CheckSuper))
					ResolveSuperClass(CheckSuper, false);

				Supermost = CheckSuper;
			}

			ClassDesc->CodeSuperClass = Supermost->CodeSuperClass;
		}
		else
		{
			auto* File = GetFileForClass(ClassDesc);
			if (File != nullptr && bShowError)
			{
				LineError(*File, ClassDesc->LineNumber, FString::Printf(TEXT("Class %s has an unknown super type %s."),
					*ClassDesc->ClassName, *ClassDesc->SuperClass));
			}
			bHasError = true;
		}
	}

	ResolvingClasses.Remove(ClassDesc);
}

void FAngelscriptPreprocessor::ParseIntoChunks(FFile& File)
{
	// Parse out all the classes from the file into chunks
	int32 ChunkStart = 0;
	int32 ChunkEnd = 0;
	int32 ChunkLine = 0;
	EChunkType ChunkType = EChunkType::Global;
	int32 RawSize = File.RawCode.Len();
	FString ChunkComment;

	bool bInComment = false;
	bool bInLineComment = false;
	bool bInBlockComment = false;
	bool bInString = false;

	TArray<FMacro> PendingMacros;
	TArray<FStreamReplace> PendingReplaces;
	TArray<FDefaultsCode> PendingDefaults;

	struct FActiveIfDef
	{
		bool bValue;
		bool bAnyBranchTaken;
		bool bHasElse;
		FString Condition;
	};
	TArray<FActiveIfDef> IfDefStack;
	TArray<FActiveIfDef> ClassIfDefs;
	bool bIfDefStackIsFalse = false;

	FMacro ParsingMacro;
	bool bIsParsingMacro = false;

	FMacro PendingClassMacro;
	bool bHasClassMacro = false;

	bool bIsEditorOnlyBlock = false;

	int32 ScopeCount = 0;
	int32 ClassExitScope = 0;
	int32 FunctionExitScope = -1;
	int32 BracketCount = 0;
	int32 MacroExitScope = -1;
	int32 PrevCommentStart = -1;
	int32 PrevCommentEnd = -1;
	int32 NameLiteralStart = -1;
	int32 FormatStringStart = -1;
	int32 LineNumber = 1;
	int32 EnumValueIndex = 0;

	TArray<FString> NamespaceStack;
	auto IsTopLevelScope = [&]() -> bool
	{
		return ScopeCount <= NamespaceStack.Num();
	};

	auto IsStartOfIdentifier = [&]() -> bool
	{
		if (ChunkEnd == 0)
			return true;
		auto PrevChar = File.RawCode[ChunkEnd-1];
		if (PrevChar >= 'a' && PrevChar <= 'z')
			return false;
		if (PrevChar >= 'A' && PrevChar <= 'Z')
			return false;
		if (PrevChar >= '0' && PrevChar <= '1')
			return false;
		if (PrevChar == '_')
			return false;
		return true;
	};

	auto UpdateEditorBlockLines = [&]
	{
		bool bHasEditorIfDef = false;
		for (auto& IfDef : IfDefStack)
		{
			if (IfDef.Condition == TEXT("EDITOR"))
			{
				bHasEditorIfDef = true;
				break;
			}
		}

		if (bHasEditorIfDef)
		{
			if (!bIsEditorOnlyBlock)
			{
				// Start a new editor block
				bIsEditorOnlyBlock = true;
#if WITH_EDITOR
				File.Module->EditorOnlyBlockLines.Add(TPair<int,int>(LineNumber, -1));
#endif
			}
		}
		else
		{
			if (bIsEditorOnlyBlock)
			{
				// The editor block has ended, record the ending line number
				bIsEditorOnlyBlock = false;
#if WITH_EDITOR
				File.Module->EditorOnlyBlockLines.Last().Value = LineNumber;
#endif
			}
		}
	};

	auto UpdateIfDefStack = [&]()
	{
		bIfDefStackIsFalse = false;
		for (auto& IfDef : IfDefStack)
		{
			if (!IfDef.bValue)
			{
				bIfDefStackIsFalse = true;
				break;
			}
		}
	};

	auto SubmitChunk = [&](bool bIncludeCurrentCharInChunk)
	{
		int SubmitEnd = ChunkEnd;
		if (bIncludeCurrentCharInChunk)
			SubmitEnd += 1;

		if (ChunkStart == SubmitEnd)
			return;

		int32 ChunkIndex = File.ChunkedCode.Add();
		FChunk& Chunk = File.ChunkedCode[ChunkIndex];

		Chunk.Type = ChunkType;
		Chunk.Content = File.RawCode.Mid(ChunkStart, SubmitEnd-ChunkStart);
		Chunk.FileLineNumber = ChunkLine;
		Chunk.ChunkStartPos = ChunkStart;
		Chunk.ChunkEndPos = SubmitEnd;

		if (NamespaceStack.Num() > 0)
		{
			Chunk.Namespace = TOptional<FString>(FString::Join(NamespaceStack, TEXT("::")));
		}
		else
		{
			Chunk.Namespace = TOptional<FString>();
		}

		Chunk.Comment = ChunkComment.TrimStartAndEnd();
		ChunkComment = TEXT("");

		if (bHasClassMacro && (ChunkType == EChunkType::Class || ChunkType == EChunkType::Struct || ChunkType == EChunkType::Enum))
		{
			Chunk.Macros.Add(PendingClassMacro);
			bHasClassMacro = false;
		}

		Chunk.Macros.Append(PendingMacros);
		for (auto& Macro : Chunk.Macros)
		{
			Macro.NameStartPos -= ChunkStart;
			Macro.NameEndPos -= ChunkStart;
			Macro.MacroStartPos -= ChunkStart;
			Macro.MacroEndPos -= ChunkStart;
		}
		PendingMacros.Empty();

		Chunk.Replacements.Append(PendingReplaces);
		PendingReplaces.Empty();

		Chunk.Defaults.Append(PendingDefaults);
		PendingDefaults.Empty();

		ChunkStart = SubmitEnd;
		if (ChunkType == EChunkType::Class || ChunkType == EChunkType::Struct)
			ClassIfDefs.Reset();

		EnumValueIndex = 0;
	};

	auto FinishMacro = [&]()
	{
		// Read arguments between brackets
		ParsingMacro.Arguments = File.RawCode.Mid(ParsingMacro.MacroStartPos + 10, ParsingMacro.MacroEndPos - ParsingMacro.MacroStartPos - 11);

		// Read backwards from end to get name, we are currently on the ; or ( that ends the variable/function name
		int32 EndOfWord = ChunkEnd-1;
		while (EndOfWord > 0 && (File.RawCode[EndOfWord] == ' ' || File.RawCode[EndOfWord] == '\t'))
			EndOfWord -= 1;
		int32 StartOfWord = EndOfWord;
		while (StartOfWord > 0 && File.RawCode[StartOfWord] != ' ' && File.RawCode[StartOfWord] != '\t')
			StartOfWord -= 1;

		ParsingMacro.NameStartPos = StartOfWord + 1;
		ParsingMacro.NameEndPos = EndOfWord + 1;
		ParsingMacro.Name = File.RawCode.Mid(StartOfWord+1, EndOfWord - StartOfWord);
		ParsingMacro.FileLineNumber = LineNumber;

		// Read backwards to parse the type of the property if it is a property
		if (ParsingMacro.Type == EMacroType::Property)
		{
			int32 EndOfType = ParsingMacro.NameStartPos - 1;
			int32 StartOfType = EndOfType;
			while (StartOfType > 0 && File.RawCode[StartOfType] != '\n' && File.RawCode[StartOfType] != ')')
				StartOfType -= 1;

			ParsingMacro.SubjectType = File.RawCode.Mid(StartOfType+1, EndOfType - StartOfType).TrimStartAndEnd();
		}

		// Function macros want to know where the end of the function block is, so record that
		if (ParsingMacro.Type == EMacroType::Function)
		{
			FunctionExitScope = ScopeCount;
		}

		PendingMacros.Add(ParsingMacro);
		ParsingMacro = FMacro();
		bIsParsingMacro = false;
	};

	auto ParseEnumValue = [&]()
	{
		EnumValueIndex += 1;

		// Don't need to parse anything if we don't have a comment, since that's all we're using this for
		if (PrevCommentStart == -1)
			return;

		FMacro Macro;
		Macro.Type = EMacroType::EnumValue;
		Macro.Comment = File.RawCode.Mid(PrevCommentStart, PrevCommentEnd-PrevCommentStart);
		Macro.SubjectIndex = EnumValueIndex - 1;

		PrevCommentStart = -1;

		PendingMacros.Add(Macro);
	};

	while (ChunkEnd < RawSize)
	{
		int16 Char = File.RawCode[ChunkEnd];

		// Handle preprocessor ifdefs
		if (Char == '#' && !bInComment)
		{
			if ((RawSize - ChunkEnd) >= 7
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#ifdef "), 7) == 0)
			{
				FString Identifier = ReadIdentifier(File, ChunkEnd + 7);
				if (!PreprocessorFlagsFromIni.Contains(Identifier) && PreprocessorFlags.Contains(Identifier))
				{
					LineError(File, LineNumber, FString::Printf(TEXT("#ifdef %s is always true, you probably want #if/#elif"), *Identifier));
					bHasError = true;
				}
				const bool bValue = PreprocessorFlags.Find(Identifier) != nullptr;
				KillRawLine(File, ChunkEnd);
				IfDefStack.Push({bValue, bValue, false, Identifier});
				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 8
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#ifndef "), 8) == 0)
			{
				FString Identifier = ReadIdentifier(File, ChunkEnd + 8);
				if (!PreprocessorFlagsFromIni.Contains(Identifier) && PreprocessorFlags.Contains(Identifier))
				{
					LineError(File, LineNumber, FString::Printf(TEXT("#ifndef %s is always false, you probably want #if/#elif"), *Identifier));
					bHasError = true;
				}
				const bool bValue = PreprocessorFlags.Find(Identifier) == nullptr;
				KillRawLine(File, ChunkEnd);
				IfDefStack.Push({bValue, bValue, false, Identifier});
				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 4
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#if "), 4) == 0)
			{
				FString PreProc = ReadIdentifier(File, ChunkEnd + 4);
				bool bValue = ParsePreProc(File, LineNumber, PreProc);
				KillRawLine(File, ChunkEnd);

				IfDefStack.Push({bValue, bValue, false, PreProc});
				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 6
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#elif "), 6) == 0)
			{
				FString PreProc = ReadIdentifier(File, ChunkEnd + 6);
				KillRawLine(File, ChunkEnd);

				if (IfDefStack.Num() == 0 || IfDefStack.Last().bHasElse)
				{
					LineError(File, LineNumber, TEXT("Invalid #elif, no matching #if found."));
					bHasError = true;
				}
				else
				{
					bool bValue = ParsePreProc(File, LineNumber, PreProc);
					IfDefStack.Last().Condition = PreProc;

					if (IfDefStack.Last().bAnyBranchTaken)
					{
						IfDefStack.Last().bValue = false;
					}
					else
					{
						IfDefStack.Last().bValue = bValue;
						if (bValue)
							IfDefStack.Last().bAnyBranchTaken = true;
					}
				}

				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 5
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#else"), 5) == 0)
			{
				KillRawLine(File, ChunkEnd);

				if (IfDefStack.Num() == 0 || IfDefStack.Last().bHasElse)
				{
					LineError(File, LineNumber, TEXT("Invalid #else, no matching #if found."));
					bHasError = true;
				}
				else
				{
					IfDefStack.Last().bValue = !IfDefStack.Last().bAnyBranchTaken;
					IfDefStack.Last().bAnyBranchTaken = true;
					IfDefStack.Last().bHasElse = true;
					if (IfDefStack.Last().Condition.StartsWith(TEXT("!")))
						IfDefStack.Last().Condition = IfDefStack.Last().Condition.Mid(1);
					else
						IfDefStack.Last().Condition = TEXT("!") + IfDefStack.Last().Condition;
				}

				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 6
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#endif"), 6) == 0)
			{
				KillRawLine(File, ChunkEnd);

				if (IfDefStack.Num() == 0)
				{
					LineError(File, LineNumber, TEXT("Invalid #endif, no matching #if found."));
					bHasError = true;
				}
				else
				{
					IfDefStack.Pop();
				}

				UpdateIfDefStack();
				UpdateEditorBlockLines();
			}
			else if ((RawSize - ChunkEnd) >= 22
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#restrict usage allow "), 22) == 0)
			{
				FString Pattern = ReadUntilWhitespace(File, ChunkEnd + 22);
				KillRawLine(File, ChunkEnd);

#if WITH_EDITOR
				FAngelscriptModuleDesc::FUsageRestriction Restriction;
				Restriction.bIsAllow = true;
				Restriction.Pattern = Pattern;

				File.Module->UsageRestrictions.Add(Restriction);
#endif

			}
			else if ((RawSize - ChunkEnd) >= 25
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("#restrict usage disallow "), 25) == 0)
			{
				FString Pattern = ReadUntilWhitespace(File, ChunkEnd + 25);
				KillRawLine(File, ChunkEnd);

#if WITH_EDITOR
				FAngelscriptModuleDesc::FUsageRestriction Restriction;
				Restriction.bIsAllow = false;
				Restriction.Pattern = Pattern;

				File.Module->UsageRestrictions.Add(Restriction);
#endif

			}
		}

		if (bIfDefStackIsFalse)
		{
			// If we're currently ifdefed out, and we're not whitespace, turn into whitespace
			if (Char != ' ' && Char != '\n' && Char != '\r' && Char != '\t')
			{
				Char = ' ';
				File.RawCode[ChunkEnd] = ' ';
			}
		}

		// Handle other preprocessor features
		switch (Char)
		{
		case 'c':
			// Does this start a class?
			if ((RawSize - ChunkEnd) >= 6
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("class"), 5) == 0
				&& IsWhitespace(File.RawCode[ChunkEnd+5])
				&& IsTopLevelScope()
				&& !bInString
				&& !bInComment
				&& IsStartOfIdentifier())
			{
				SubmitChunk(false);

				// Record the comment that we should add to this chunk
				if (PrevCommentStart != -1)
				{
					ChunkComment = File.RawCode.Mid(PrevCommentStart, PrevCommentEnd-PrevCommentStart);
					PrevCommentStart = -1;
				}

				// Class chunk has started
				ClassIfDefs = IfDefStack;
				ChunkType = EChunkType::Class;
				ChunkLine = LineNumber;
				ClassExitScope = ScopeCount;
			}
		break;
		case 's':
			// Does this start a struct?
			if ((RawSize - ChunkEnd) >= 7
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("struct"), 6) == 0
				&& IsWhitespace(File.RawCode[ChunkEnd+6])
				&& IsTopLevelScope()
				&& !bInComment
				&& !bInString
				&& IsStartOfIdentifier())
			{
				SubmitChunk(false);

				// Record the comment that we should add to this chunk
				if (PrevCommentStart != -1)
				{
					ChunkComment = File.RawCode.Mid(PrevCommentStart, PrevCommentEnd-PrevCommentStart);
					PrevCommentStart = -1;
				}

				// Class chunk has started
				ClassIfDefs = IfDefStack;
				ChunkType = EChunkType::Struct;
				ChunkLine = LineNumber;
				ClassExitScope = ScopeCount;
			}
		break;
		case 'U':
			if ((RawSize - ChunkEnd) >= 10
				&& ((IsTopLevelScope() && ChunkType == EChunkType::Global)
					|| (ScopeCount == ClassExitScope + 1 && (ChunkType == EChunkType::Class || ChunkType == EChunkType::Struct || ChunkType == EChunkType::Enum)))
				&& !bInComment
				&& !bInString
				&& !bIsParsingMacro
				&& IsStartOfIdentifier())
			{
				if (FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("UPROPERTY("), 10) == 0
					&& (ChunkType == EChunkType::Class || ChunkType == EChunkType::Struct))
				{
					bIsParsingMacro = true;
					ParsingMacro.Type = EMacroType::Property;
				}
				else if (FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("UFUNCTION("), 10) == 0
					&& (ChunkType != EChunkType::Enum))
				{
					bIsParsingMacro = true;
					ParsingMacro.Type = EMacroType::Function;
				}
				else if (ChunkType == EChunkType::Enum
					&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("UMETA("), 6) == 0)
				{
					ParsingMacro.Type = EMacroType::EnumMeta;

					int32 CloseBracket = FindScopeCloseBracket(File.RawCode, ChunkEnd + 6 - 1);
					if (CloseBracket != -1)
					{
						ParsingMacro.MacroStartPos = ChunkEnd;
						ParsingMacro.MacroEndPos = CloseBracket + 1;
						ParsingMacro.FileLineNumber = LineNumber;
						ParsingMacro.Arguments = File.RawCode.Mid(ChunkEnd + 6, CloseBracket - ChunkEnd - 6);
						ParsingMacro.SubjectIndex = EnumValueIndex;
						PendingMacros.Add(ParsingMacro);

						ChunkEnd = CloseBracket;
					}

					ParsingMacro = FMacro();
					bIsParsingMacro = false;
				}
				else if (ChunkType == EChunkType::Global)
				{
					bool bIsClass = FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("UCLASS("), 7) == 0;
					bool bIsStruct = FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("USTRUCT("), 8) == 0;
					bool bIsEnum = FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("UENUM("), 6) == 0;

					if (bIsClass || bIsStruct)
					{
						// Immediately parse this macro, since we don't want to parse the name
						// following it.
						PendingClassMacro = FMacro();
						PendingClassMacro.Type = EMacroType::Class;

						int32 Offset = bIsClass ? 7 : 8;
						
						int32 CloseBracket = FindScopeCloseBracket(File.RawCode, ChunkEnd + Offset - 1);
						if (CloseBracket != -1)
						{
							PendingClassMacro.MacroStartPos = ChunkEnd;
							PendingClassMacro.MacroEndPos = CloseBracket + 1;
							PendingClassMacro.FileLineNumber = LineNumber;
							PendingClassMacro.Arguments = File.RawCode.Mid(ChunkEnd + Offset, CloseBracket - ChunkEnd - Offset);
							bHasClassMacro = true;

							ChunkEnd = CloseBracket;
						}

						bIsParsingMacro = false;
					}
					else if (bIsEnum)
					{
						// Immediately parse this macro, since we don't want to parse the name
						// following it.
						PendingClassMacro = FMacro();
						PendingClassMacro.Type = EMacroType::Enum;

						int32 CloseBracket = FindScopeCloseBracket(File.RawCode, ChunkEnd + 6 - 1);
						if (CloseBracket != -1)
						{
							PendingClassMacro.MacroStartPos = ChunkEnd;
							PendingClassMacro.MacroEndPos = CloseBracket + 1;
							PendingClassMacro.FileLineNumber = LineNumber;
							PendingClassMacro.Arguments = File.RawCode.Mid(ChunkEnd + 6, CloseBracket - ChunkEnd - 6);
							bHasClassMacro = true;

							ChunkEnd = CloseBracket;
						}

						bIsParsingMacro = false;
					}
				}

				if (bIsParsingMacro)
				{
					if (PrevCommentStart != -1)
					{
						ParsingMacro.Comment = File.RawCode.Mid(PrevCommentStart, PrevCommentEnd-PrevCommentStart).TrimStartAndEnd();
						PrevCommentStart = -1;
					}
					MacroExitScope = -1;
					ParsingMacro.MacroStartPos = ChunkEnd;

					if (IfDefStack.Num() != 0)
					{
						if (ParsingMacro.Type == EMacroType::Property || ParsingMacro.Type == EMacroType::Function)
						{
							// Check if we are allowed to specify these ifdefs
							for (int i = ClassIfDefs.Num(), Count = IfDefStack.Num(); i < Count; ++i)
							{
								const bool bIsNegated = (IfDefStack[i].Condition.Len() != 0 && IfDefStack[i].Condition[0] == '!');
								const bool bIsEditorData = IfDefStack[i].Condition == TEXT("EDITOR") || IfDefStack[i].Condition == TEXT("EDITORONLY_DATA");

								// If this is a flag that's configured as a constant regardless of build configuration, we allow it to affect reflection,
								// since there is no chance of it breaking cooked content compatibility.
								bool bIsConstantConfigFlag = false;
								if (bIsNegated)
								{
									FString BaseFlagName = IfDefStack[i].Condition.Mid(1);
									if (PreprocessorFlagsFromIni.Contains(BaseFlagName))
										bIsConstantConfigFlag = true;
									else if (!PreprocessorFlags.Contains(BaseFlagName))
										bIsConstantConfigFlag = true;
								}
								else
								{
									if (PreprocessorFlagsFromIni.Contains(IfDefStack[i].Condition))
										bIsConstantConfigFlag = true;
									else if (!PreprocessorFlags.Contains(IfDefStack[i].Condition))
										bIsConstantConfigFlag = true;
								}

								if (!bIsEditorData && !bIsConstantConfigFlag)
								{
									// Invalid condition
									LineError(File, LineNumber, TEXT("Cannot put a UPROPERTY or UFUNCTION inside preprocessor conditions other than EDITOR or flags declared in project settings."));
									bHasError = true;
									break;
								}
							}

							// Check if we are an editor-only property or function
							for (const auto& Elem : IfDefStack)
							{
								if (Elem.Condition == TEXT("EDITOR") || Elem.Condition == TEXT("EDITORONLY_DATA"))
								{
									ParsingMacro.bEditorOnly = true;
									break;
								}
							}
						}
					}
				}
			}
		break;
		case 'd':
			if (ChunkType == EChunkType::Class
				&& ScopeCount == ClassExitScope + 1
				&& !bInComment
				&& !bInString
				&& !bIsParsingMacro
				&& IsStartOfIdentifier())
			{
				if ((RawSize - ChunkEnd) >= 8
					&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("default"), 7) == 0
					&& IsWhitespace(File.RawCode[ChunkEnd + 7]))
				{
					FDefaultsCode Code;
					Code.StartPos = ChunkEnd - ChunkStart;
					PendingDefaults.Add(Code);
					break;
				}
			}
		case 'e':
			if (IsTopLevelScope()
				&& ChunkType == EChunkType::Global
				&& !bInComment
				&& !bInString
				&& IsStartOfIdentifier())
			{
				bool bIsDelegate = false;
				bool bIsMulticast = false;

				if ((RawSize - ChunkEnd) >= 6
					&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("event"), 5) == 0
					&& IsWhitespace(File.RawCode[ChunkEnd + 5]))
				{
					bIsDelegate = true;
					bIsMulticast = true;
				}
				else if ((RawSize - ChunkEnd) >= 9
					&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("delegate"), 8) == 0
					&& IsWhitespace(File.RawCode[ChunkEnd + 8]))
				{
					bIsDelegate = true;
				}

				if (bIsDelegate)
				{
					bool bInvalidDelegate = false;

					int32 BracketPos = ChunkEnd;
					while (BracketPos < File.RawCode.Len() && File.RawCode[BracketPos] != '(')
						BracketPos += 1;

					int32 CloseBracket = FindScopeCloseBracket(File.RawCode, BracketPos);
					if (CloseBracket != -1)
					{
						int32 SemiColon = FindSemicolonDirectlyAfter(File.RawCode, CloseBracket);
						if (SemiColon != -1)
						{
							FDelegateDesc Delegate;
							Delegate.ChunkIndex = File.ChunkedCode.Num();
							Delegate.StartPosInChunk = ChunkEnd - ChunkStart;
							Delegate.EndPosInChunk = SemiColon - ChunkStart;
							Delegate.BracketPos = BracketPos - ChunkStart;
							Delegate.bIsMulticast = bIsMulticast;
							Delegate.FileLineNumber = LineNumber;

							File.Delegates.Add(Delegate);
						}
					}
				}

				if ((RawSize - ChunkEnd) >= 5
					&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("enum"), 4) == 0
					&& IsWhitespace(File.RawCode[ChunkEnd + 4]))
				{
					SubmitChunk(false);

					// Record the comment that we should add to this chunk
					if (PrevCommentStart != -1)
					{
						ChunkComment = File.RawCode.Mid(PrevCommentStart, PrevCommentEnd-PrevCommentStart);
						PrevCommentStart = -1;
					}

					// Class chunk has started
					ChunkType = EChunkType::Enum;
					ChunkLine = LineNumber;
					ClassExitScope = ScopeCount;
				}
			}
		break;
		case ',':
			if (ChunkType == EChunkType::Enum
				&& !bInComment
				&& !bInString
				&& !bIsParsingMacro
				&& BracketCount == 0)
			{
				ParseEnumValue();
			}
		break;
		case 'n':
			// Name literals
			if ((RawSize - ChunkEnd) >= 3
				&& !bInComment
				&& !bInString
				&& File.RawCode[ChunkEnd+1] == '"')
			{
				NameLiteralStart = ChunkEnd;
			}
			else if ((RawSize - ChunkEnd) >= 10
				&& FCString::Strncmp(&File.RawCode[ChunkEnd], TEXT("namespace"), 9) == 0
				&& IsWhitespace(File.RawCode[ChunkEnd+9])
				&& IsTopLevelScope()
				&& !bInString
				&& !bInComment)
			{
				NamespaceStack.Add(ReadIdentifier(File, ChunkEnd+10));
			}
		break;
		case 'f':
			// F-Strings
			if ((RawSize - ChunkEnd) >= 3
				&& !bInComment
				&& !bInString
				&& File.RawCode[ChunkEnd+1] == '"')
			{
				FormatStringStart = ChunkEnd;
			}
		break;
		case '(':
			if (bInString || bInComment)
				break;
			if (bIsParsingMacro)
			{
				if (MacroExitScope == -1)
					MacroExitScope = BracketCount;
				else if (MacroExitScope == BracketCount)
					FinishMacro();
			}
			BracketCount += 1;
		break;
		case ')':
			if (bInString || bInComment)
				break;
			BracketCount -= 1;
			if (BracketCount < 0)
				BracketCount = 0;
			if (bIsParsingMacro && MacroExitScope == BracketCount && ParsingMacro.MacroEndPos == -1)
				ParsingMacro.MacroEndPos = ChunkEnd + 1;
		break;
		case ';':
			if (bInString || bInComment)
				break;
			if (bIsParsingMacro && MacroExitScope == BracketCount)
				FinishMacro();
			PrevCommentStart = -1;
		break;
		case '=':
			if (bInString || bInComment)
				break;
			if (bIsParsingMacro && MacroExitScope == BracketCount)
				FinishMacro();
		break;
		case '{':
			if (bInString || bInComment)
				break;
			PrevCommentStart = -1;
			ScopeCount += 1;
		break;
		case '}':
			if (bInString || bInComment)
				break;
			if (ChunkType == EChunkType::Enum)
				ParseEnumValue();
			if (IsTopLevelScope() && NamespaceStack.Num() != 0)
				NamespaceStack.Pop();
			PrevCommentStart = -1;
			ScopeCount -= 1;
			if (ScopeCount < 0)
				ScopeCount = 0;
			if (ScopeCount == ClassExitScope
				&& (ChunkType == EChunkType::Class || ChunkType == EChunkType::Struct || ChunkType == EChunkType::Enum))
			{
				// Class chunk is now over, include the closing brace 
				SubmitChunk(true);
				ChunkLine = LineNumber;
				ChunkType = EChunkType::Global;
			}
			if (FunctionExitScope != -1 && ScopeCount == FunctionExitScope)
			{
				PendingMacros.Last().FunctionEndChunkOffset = ChunkEnd + 1 - ChunkStart;
				PendingMacros.Last().FunctionEndLineNumber = LineNumber;
				FunctionExitScope = -1;
			}
		break;
		case '/':
			// Deal with comment starts
			if ((RawSize - ChunkEnd) >= 2 && !bInComment && !bInString)
			{
				int64 NextChar = File.RawCode[ChunkEnd + 1];
				if(NextChar == '/')
				{
					bInLineComment = true;
					bInComment = true;

					PrevCommentStart = ChunkEnd;

					// Skip next slash
					ChunkEnd += 1;
				}
				else if(NextChar == '*')
				{
					bInBlockComment = true;
					bInComment = true;

					PrevCommentStart = ChunkEnd;

					// Skip next star
					ChunkEnd += 1;
				}
			}
		break;
		case '*':
			if ((RawSize - ChunkEnd) >= 2 && bInBlockComment)
			{
				int64 NextChar = File.RawCode[ChunkEnd + 1];
				if(NextChar == '/')
				{
					bInBlockComment = false;
					bInComment = false;

					PrevCommentEnd = ChunkEnd+2;

					// Skip next slash
					ChunkEnd += 1;
				}
			}
		break;
		case '"':
			if (!bInComment)
			{
				bool bInEscapeSequence = false;
				if (bInString)
				{
					int CheckEscape = ChunkEnd-1;
					while (CheckEscape >= 0 && File.RawCode[CheckEscape] == '\\')
					{
						bInEscapeSequence = !bInEscapeSequence;
						CheckEscape -= 1;
					}
				}

				if (!bInEscapeSequence)
				{
					bInString = !bInString;

					if (!bInString && NameLiteralStart != -1 && NameLiteralStart >= ChunkStart)
					{
						PendingReplaces.Add({NameLiteralStart-ChunkStart, ChunkEnd-ChunkStart+1,
							GenerateStaticName(File, File.RawCode.Mid(NameLiteralStart+2, ChunkEnd - NameLiteralStart - 2))});
						NameLiteralStart = -1;
					}
					else if (!bInString && FormatStringStart != -1 && FormatStringStart >= ChunkStart)
					{
						FString FormatStr = File.RawCode.Mid(FormatStringStart + 2, ChunkEnd - FormatStringStart - 2);
						FString GeneratedCode = GenerateFormatString(File, FormatStr);
						PendingReplaces.Add({FormatStringStart-ChunkStart, ChunkEnd-ChunkStart+1, GeneratedCode});
						FormatStringStart = -1;
					}
				}
			}
		break;
		case '\n':
			if (bInLineComment)
			{
				bInLineComment = false;
				bInComment = false;
				PrevCommentEnd = ChunkEnd;
			}

			NameLiteralStart = -1;
			FormatStringStart = -1;
			++LineNumber;
		break;
		}
		ChunkEnd += 1;
	}

	// Submit the last chunk in the file
	SubmitChunk(false);

	// Show an error if we never closed our preprocessor condition

	if (IfDefStack.Num() != 0)
	{
		LineError(File, LineNumber, TEXT("Preceding preprocessor #if/#ifdef/#else was not closed, missing #endif."));
		bHasError = true;
	}
}

void FAngelscriptPreprocessor::ProcessReplacements(FFile& File, FChunk& Chunk)
{
	// Deal with stream replaces in the chunk
	if (Chunk.Replacements.Num() == 0)
		return;

	Chunk.Replacements.Sort();

	FString NewCode;
	NewCode.Reset(Chunk.Content.Len());

	for (auto& Def : Chunk.Defaults)
		Def.NewStartPos = Def.StartPos;

	const TCHAR* ContentPtr = &Chunk.Content[0];
	int32 StartPos = 0;
	for (auto& Repl : Chunk.Replacements)
	{
		if (Repl.StartPos > StartPos)
			NewCode.Append(ContentPtr + StartPos, Repl.StartPos - StartPos);
		NewCode += Repl.Replacement;
		StartPos = Repl.EndPos;

		for(auto& Def : Chunk.Defaults)
		{
			if (Def.StartPos >= Repl.StartPos)
				Def.NewStartPos += (Repl.Replacement.Len()) - (Repl.EndPos - Repl.StartPos);
		}
	}

	for (auto& Def : Chunk.Defaults)
		Def.StartPos = Def.NewStartPos;

	if (Chunk.Content.Len() > StartPos)
		NewCode.Append(ContentPtr + StartPos, Chunk.Content.Len() - StartPos);

	Chunk.Content = MoveTemp(NewCode);
	Chunk.Replacements.Empty();
}

void FAngelscriptPreprocessor::CondenseFromChunks(FFile& File)
{
	// Pre-allocate string size for all chunks first
	int32 CodeSize = 0;
	for (auto& Chunk : File.ChunkedCode)
		CodeSize += Chunk.Content.Len();
	for (FString& Generated : File.GeneratedCode)
		CodeSize += Generated.Len() + 2;
	File.ProcessedCode.Reset(CodeSize);

	// Concat all chunks into final code
	for (auto& Chunk : File.ChunkedCode)
	{
		ProcessReplacements(File, Chunk);
		File.ProcessedCode += Chunk.Content;
	}

	// Concat generated code at the end
	for (FString& Generated : File.GeneratedCode)
	{
		File.ProcessedCode += TEXT("\n\n");
		File.ProcessedCode += Generated;
	}
}

bool FAngelscriptPreprocessor::ShallowCheckIsInComment(const FString& Content, int Offset)
{
	// This is a very limited check if a particular offset is within a comment or not,
	// it will not catch all cases, but should avoid issues from the most common ones

	// Find the start of the line
	int LineStart = Offset;
	while (LineStart > 0)
	{
		if (Content[LineStart] == '\n')
			break;
		LineStart -= 1;
	}

	bool bIsEscaped = false;
	bool bIsInString = false;
	bool bIsInChar = false;
	bool bIsBlockComment = false;

	// Forward search for comments and string literals
	for (int CharIndex = LineStart; CharIndex < Offset; ++CharIndex)
	{
		int64 Chr = Content[CharIndex];
		if (Chr == '\\' && (bIsInString || bIsInChar) && !bIsEscaped)
		{
			bIsEscaped = true;
			continue;
		}

		if (Chr == '"' && !bIsEscaped)
		{
			bIsInString = !bIsInString;
		}
		else if (Chr == '\'' && !bIsEscaped)
		{
			bIsInChar = !bIsInChar;
		}
		else if (Chr == '/' && !bIsInString && !bIsInChar)
		{
			if (CharIndex > LineStart && Content[CharIndex-1] == '/')
			{
				// Line comment, can return immediately
				return true;
			}
			else if (bIsBlockComment && CharIndex > LineStart && Content[CharIndex-1] == '*')
			{
				// End of block comment
				bIsBlockComment = false;
			}
			else if (!bIsBlockComment && CharIndex+1 < Offset && Content[CharIndex+1] == '*')
			{
				// Start of block comment
				bIsBlockComment = true;
			}
		}
		else if (Chr == '#' && !bIsBlockComment && !bIsInString && !bIsInChar)
		{
			// Pre-processor directive, ignore the rest of the line
			return true;
		}

		bIsEscaped = false;
	}

	return bIsInString || bIsBlockComment;
}

void FAngelscriptPreprocessor::PostProcessRangeBasedFor(FFile& File)
{
	if (GAngelscriptNewIterators != 0)
		return;

	static const FRegexPattern RangeBasedForPattern(TEXT("for(\\s*)\\(([^:;{]*):([^:;{\n][^;{\n]*)\\)(\\s*)(\\{|.*;)"));

	FString NewCode;
	int32 PrevPosition = 0;
	FRegexMatcher MatchFor(RangeBasedForPattern, File.ProcessedCode);

	while (MatchFor.FindNext())
	{
		int32 ForStart = MatchFor.GetMatchBeginning();
		if (ShallowCheckIsInComment(File.ProcessedCode, ForStart))
			continue;
		if (ForStart > PrevPosition)
			NewCode += File.ProcessedCode.Mid(PrevPosition, ForStart - PrevPosition);

		FString FinalGroup = MatchFor.GetCaptureGroup(5);
		bool bSingleLine = (FinalGroup != TEXT("{"));

		FString StoreType = MatchFor.GetCaptureGroup(2);
		StoreType.TrimStartAndEndInline();

		int32 StartOfName = StoreType.Len() - 1;
		while (StartOfName >= 0)
		{
			int16 Char = StoreType[StartOfName];
			if ((Char >= 'a' && Char <= 'z')
				|| (Char >= 'A' && Char <= 'Z')
				|| (Char >= '0' && Char <= '9')
				|| Char == '_')
			{
				--StartOfName;
				continue;
			}
			else
			{
				break;
			}
		}

		// If the iterator type is a struct, we should always use a const ref here for performance reasons
		StoreType = StoreType.Mid(0, StartOfName) + TEXT(" __auto_constref_type") + StoreType.Mid(StartOfName);

		FString SuffixGroup;
		if (!bSingleLine)
		{
			SuffixGroup = MatchFor.GetCaptureGroup(4);
		}
		else if (FinalGroup == TEXT(";"))
		{
			SuffixGroup = TEXT(";");
		}

		NewCode += FString::Printf(
			TEXT("for%s(auto _Iterator = %s.Iterator();")
			TEXT("_Iterator.CanProceed; )")
			TEXT("%s{ %s = _Iterator.Proceed();"),

			*MatchFor.GetCaptureGroup(1),
			*MatchFor.GetCaptureGroup(3),
			*SuffixGroup,
			*StoreType
		);

		if (bSingleLine)
		{
			NewCode += MatchFor.GetCaptureGroup(4);
			NewCode += FinalGroup;
			NewCode += TEXT("}");
		}

		PrevPosition = MatchFor.GetMatchEnding();
	}

	// If no matches were found at all, don't need to do anything
	if (PrevPosition == 0)
		return;

	if(PrevPosition < File.ProcessedCode.Len())
		NewCode += File.ProcessedCode.Mid(PrevPosition);
	File.ProcessedCode = NewCode;
}

int FAngelscriptPreprocessor::GetLineNumberFromCodeOffset(const FString& Code, int Offset)
{
	int Lines = 1;
	for (int i = 0, Count = Code.Len(); i < Count && i < Offset; ++i)
	{
		if (Code[i] == '\n')
			Lines += 1;
	}
	return Lines;
}

void FAngelscriptPreprocessor::PostProcessLiteralAssets(FFile& File)
{
	static const FRegexPattern LiteralAssetsPattern(TEXT("asset\\s+([A-Za-z0-9_]+)\\s+of\\s+([A-Za-z0-9_]+)\\s*($|\\r|\\n)"));

	FString NewCode;
	int32 PrevPosition = 0;
	FRegexMatcher MatchSettings(LiteralAssetsPattern, File.ProcessedCode);

	while (MatchSettings.FindNext())
	{
		int32 AssetStart = MatchSettings.GetMatchBeginning();
		if (ShallowCheckIsInComment(File.ProcessedCode, AssetStart))
			continue;

		if (AssetStart > PrevPosition)
		{
			NewCode.Reserve(File.ProcessedCode.Len() + 500);
			NewCode += File.ProcessedCode.Mid(PrevPosition, AssetStart - PrevPosition);
		}

		FString AssetName = MatchSettings.GetCaptureGroup(1);
		FString SettingsType = MatchSettings.GetCaptureGroup(2);

		if (File.Module->DeclaredLiteralAssets.Contains(AssetName))
		{
			int AssetLineNumber = GetLineNumberFromCodeOffset(NewCode, NewCode.Len());
			LineError(File, AssetLineNumber, FString::Printf(TEXT("Literal asset %s is already declared in this module"), *AssetName));
			bHasError = true;
		}
		else if (auto* PreprocessingModule = PreprocessingLiteralAssets.Find(AssetName))
		{
			int AssetLineNumber = GetLineNumberFromCodeOffset(NewCode, NewCode.Len());
			LineError(File, AssetLineNumber, FString::Printf(TEXT("Cannot declare literal asset %s in module %s. An asset with this name already exists in module %s."),
				*AssetName, *File.Module->ModuleName, *(*PreprocessingModule)->ModuleName));
			bHasError = true;
		}
		else
		{
			auto ExistingModule = FAngelscriptManager::Get().GetModuleContainingLiteralAsset(AssetName);
			if (ExistingModule.IsValid())
			{
				// If we're not preprocessing this module, and the class already exists, error.
				if (!IsPreprocessingModule(ExistingModule->ModuleName))
				{
					int AssetLineNumber = GetLineNumberFromCodeOffset(NewCode, NewCode.Len());
					LineError(File, AssetLineNumber, FString::Printf(TEXT("Cannot declare literal asset %s in module %s. An asset with this name already exists in module %s."),
						*AssetName, *File.Module->ModuleName, *ExistingModule->ModuleName));
					bHasError = true;
				}
			}
		}

		const TCHAR* FormatStr =
			TEXT("{Type} __Asset_{Name};")
			TEXT("{Type} Get{Name}() property")
			TEXT("{")
			TEXT("	if (__Asset_{Name} != nullptr)")
			TEXT("		return __Asset_{Name};")
			TEXT("")
			TEXT("	__Asset_{Name} = Cast<{Type}>(__CreateLiteralAsset({Type}, \"{Name}\"));")
			TEXT("	if (__Asset_{Name} == nullptr) return nullptr;")
			TEXT("	__Init_{Name}(__Asset_{Name});")
			TEXT("	__PostLiteralAssetSetup(__Asset_{Name}, \"{Name}\");")
			TEXT("	return __Asset_{Name};")
			TEXT("} ")
			TEXT("void __Init_{Name}({Type} {Name}) external_implicit_this\n");


		TMap<FString, FStringFormatArg> Args;
		Args.Add(TEXT("Name"), AssetName);
		Args.Add(TEXT("Type"), SettingsType);

		NewCode += FString::Format(FormatStr, Args);
		PrevPosition = MatchSettings.GetMatchEnding();

		// Execute the getter as a postinit function so we ensure the asset is created
		File.Module->PostInitFunctions.Add(TEXT("Get") + AssetName);
		File.Module->DeclaredLiteralAssets.Add(AssetName);
		PreprocessingLiteralAssets.Add(AssetName, File.Module);
	}

	// If no matches were found at all, don't need to do anything
	if (PrevPosition == 0)
		return;

	if(PrevPosition < File.ProcessedCode.Len())
		NewCode += File.ProcessedCode.Mid(PrevPosition);
	File.ProcessedCode = NewCode;
}

int32 FAngelscriptPreprocessor::FindScopeCloseBracket(const FString& InString, int32 OpenBracketPos)
{
	int32 Len = InString.Len();
	int32 Pos = OpenBracketPos + 1;
	int32 BracketDepth = 1;

	while (Pos < Len)
	{
		auto Char = InString[Pos];
		if (Char == '(')
			BracketDepth += 1;
		if (Char == ')')
		{
			BracketDepth -= 1;
			if (BracketDepth == 0)
			{
				return Pos;
			}
		}
		Pos += 1;
	}

	return -1;
}

int32 FAngelscriptPreprocessor::FindSemicolonDirectlyAfter(const FString& InString, int32 AfterPos)
{
	int32 Len = InString.Len();
	int32 Pos = AfterPos + 1;

	while (Pos < Len)
	{
		auto Char = InString[Pos];
		if (Char == ';')
		{
			return Pos;
		}
		else if (Char == '\t' || Char == ' ' || Char == '\n' || Char == '\r')
		{
			Pos += 1;
			continue;
		}
		else
		{
			return -1;
		}
	}

	return -1;
}

FAngelscriptPreprocessor::FChunk& FAngelscriptPreprocessor::ResolveFilePos(FFile& File, int32 FilePos, int32& OutChunkPos)
{
	for (FChunk& Chunk : File.ChunkedCode)
	{
		if (FilePos >= Chunk.ChunkStartPos && FilePos < Chunk.ChunkEndPos)
		{
			OutChunkPos = FilePos - Chunk.ChunkStartPos;
			return Chunk;
		}
	}

	OutChunkPos = -1;
	return File.ChunkedCode[0];
}

void FAngelscriptPreprocessor::ReplaceWithBlankFilePos(FFile& File, int32 StartPos, int32 EndPos)
{
	int32 StartInChunk;
	FChunk& Chunk = ResolveFilePos(File, StartPos, StartInChunk);

	int32 EndInChunk;
	FChunk& ChunkEnd = ResolveFilePos(File, EndPos, EndInChunk);

	if (!ensure(&Chunk == &ChunkEnd))
		return;

	ReplaceWithBlank(Chunk, StartInChunk, EndInChunk);
}

void FAngelscriptPreprocessor::FileWideError(FFile& File, const FString& Message)
{
                                                                                                                
	File.Module->bCompileError = true;
                                                                                                              
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = 1;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = true;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);
}

void FAngelscriptPreprocessor::FileWideWarning(FFile& File, const FString& Message)
{
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = 1;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = false;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);
}

void FAngelscriptPreprocessor::LineError(FFile& File, int32 Line, const FString& Message)
{
                                                                                                            
	File.Module->bCompileError = true;
                                                                                                          
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = Line;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = true;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);
}

void FAngelscriptPreprocessor::LineWarning(FFile& File, int32 Line, const FString& Message)
{
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = Line;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = false;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);
}

void FAngelscriptPreprocessor::ChunkError(FFile& File, FChunk& Chunk, const FString& Message)
{
                                                                                                             
	File.Module->bCompileError = true;
                                                                                                           
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = Chunk.FileLineNumber;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = true;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);
}

void FAngelscriptPreprocessor::MacroError(FFile& File, FMacro& Macro, const FString& Message)
{
                                                                                                             
	File.Module->bCompileError = true;
                                                                                                           
	FAngelscriptManager::FDiagnostic Diagnostic;
	Diagnostic.Message = Message;
	Diagnostic.Row = Macro.FileLineNumber;
	Diagnostic.Column = 1;
	Diagnostic.bIsError = true;
	Diagnostic.bIsInfo = false;

	FAngelscriptManager::Get().ScriptCompileError(File.AbsoluteFilename, Diagnostic);

}

FString FAngelscriptPreprocessor::ReadIdentifier(FFile& File, int32 Pos)
{
	int32 StartPos = Pos;
	int32 EndPos = Pos;
	while (EndPos < File.RawCode.Len())
	{
		int16 Char = File.RawCode[EndPos];
		if (Char == '\n' || Char == '/' || Char == '{')
			break;
		EndPos += 1;
	}

	return File.RawCode.Mid(StartPos, EndPos - StartPos).TrimStartAndEnd();
}

FString FAngelscriptPreprocessor::ReadUntilWhitespace(FFile& File, int32 Pos)
{
	int32 StartPos = Pos;
	int32 EndPos = Pos;
	while (EndPos < File.RawCode.Len())
	{
		int16 Char = File.RawCode[EndPos];
		if (Char == '\n' || Char == '\t' || Char == ' ')
			break;
		EndPos += 1;
	}

	return File.RawCode.Mid(StartPos, EndPos - StartPos).TrimStartAndEnd();
}

bool FAngelscriptPreprocessor::ParsePreProc(FFile& File, int32 LineNumber, const FString& PreProc)
{
	FString Lookup = PreProc;
	bool bNegate = false;
	if (PreProc.Len() != 0 && PreProc[0] == '!')
	{
		bNegate = true;
		Lookup = Lookup.Mid(1);
	}

	bool* bValue = PreprocessorFlags.Find(Lookup);
	if (bValue == nullptr)
	{
		LineError(File, LineNumber, FString::Printf(TEXT("Invalid preprocessor condition: %s"), *PreProc));
		bHasError = true;
		return false;
	}

	return *bValue != bNegate;
}

void FAngelscriptPreprocessor::KillRawLine(FFile& File, int32 FromPos)
{
	int32 EndPos = FromPos;
	while (EndPos < File.RawCode.Len())
	{
		int16 Char = File.RawCode[EndPos];
		if (Char == '\n' || Char == '/')
			break;

		File.RawCode[EndPos] = ' ';
		EndPos += 1;
	}
}

void FAngelscriptPreprocessor::StripCommentsFromLine(FString& Line)
{
	bool bInLineComment = false;
	bool bInString = false;
	bool bInBlockComment = false;

	for (int32 i = 0, Count = Line.Len(); i < Count; ++i)
	{
		auto Char = Line[i];

		if (Char == '"' && !bInLineComment && !bInBlockComment)
		{
			bInString = !bInString;
		}

		if (Char == '/' && (i < Count-1) && !bInString && !bInLineComment && !bInBlockComment)
		{
			auto NextChar = Line[i + 1];
			if (NextChar == '/')
			{
				bInLineComment = true;
			}
			else if (NextChar == '*')
			{
				bInBlockComment = true;
			}
		}

		if (Char == '\n' && bInLineComment)
		{
			bInLineComment = false;
		}

		if (bInBlockComment || bInLineComment)
		{
			Line[i] = ' ';
		}

		if (Char == '*' && (i < Count - 1) && bInBlockComment)
		{
			auto NextChar = Line[i + 1];
			if (NextChar == '/')
			{
				Line[i + 1] = ' ';
				bInBlockComment = false;
			}
		}
	}
}

void FAngelscriptPreprocessor::AddEditorOnlyLines(FFile& File, int32 StartLineNumber, int32 EndLineNumber)
{
#if WITH_EDITOR
	File.Module->EditorOnlyBlockLines.Add(TPair<int,int>(StartLineNumber, EndLineNumber));
#endif
}
