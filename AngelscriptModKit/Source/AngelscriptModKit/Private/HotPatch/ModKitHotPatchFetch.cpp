// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch fetch implementation .

#include "HotPatch/ModKitHotPatchFetch.h"

#include "Shared/Delivery/ModKitPatchManifest.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "HttpManager.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/ScopeLock.h"
#include "Serialization/Archive.h"

namespace
{
#if WITH_DEV_AUTOMATION_TESTS
	// Only automation on the game thread installs this transport for its scope.
	struct FPatchHttpStubResponse
	{
		int32 Status = 200;
		TArray<uint8> Body;
		bool bTimeout = false;
	};
	TFunction<FPatchHttpStubResponse(const FString&)>* PatchHttpStub = nullptr;
#endif

	bool PatchFetchReadLocalTree(const FString& RootDir, int64 MaxBytes, FModKitFetchedPatch& Out,
		FString& OutReason, FString& OutDetail)
	{
		IFileManager& FM = IFileManager::Get();
		if (!FM.DirectoryExists(*RootDir))
		{
			OutReason = TEXT("not-found");
			OutDetail = RootDir;
			return false;
		}

		const FString ManifestPath = RootDir / TEXT("Manifest.json");
		const int64 ManifestSize = FM.FileSize(*ManifestPath);
		if (ManifestSize > MaxBytes)
		{
			OutReason = TEXT("size");
			OutDetail = TEXT("manifest");
			return false;
		}
		if (!FFileHelper::LoadFileToString(Out.ManifestText, *ManifestPath))
		{
			OutReason = TEXT("not-found");
			OutDetail = TEXT("Manifest.json");
			return false;
		}

		TArray<FString> RelPaths;
		TArray<FString> AllFiles;
		FM.FindFilesRecursive(AllFiles, *RootDir, TEXT("*"), true, false);
		for (const FString& Abs : AllFiles)
		{
			FString Rel = Abs;
			if (!Rel.RemoveFromStart(RootDir / TEXT("")))
			{
				continue;
			}
			Rel.ReplaceInline(TEXT("\\"), TEXT("/"));
			if (Rel == TEXT("Manifest.json"))
			{
				continue;
			}
			RelPaths.Add(Rel);
		}
		RelPaths.Sort();

		// Size cap is enforced on declared file sizes before any body is read.
		int64 Total = FM.FileSize(*ManifestPath);
		for (const FString& Rel : RelPaths)
		{
			const int64 Size = FM.FileSize(*(RootDir / Rel));
			if (Size < 0)
			{
				OutReason = TEXT("not-found");
				OutDetail = Rel;
				return false;
			}
			Total += Size;
		}
		if (MaxBytes > 0 && Total > MaxBytes)
		{
			OutReason = TEXT("size");
			OutDetail = FString::Printf(TEXT("%lld>%lld"), (long long)Total, (long long)MaxBytes);
			return false;
		}

		for (const FString& Rel : RelPaths)
		{
			TArray<uint8> Bytes;
			if (!FFileHelper::LoadFileToArray(Bytes, *(RootDir / Rel)))
			{
				OutReason = TEXT("not-found");
				OutDetail = Rel;
				return false;
			}
			Out.Files.Add(Rel, MoveTemp(Bytes));
		}
		Out.TotalBytes = Total;
		return true;
	}

	class FPatchBoundedResponse final : public FArchive
	{
	public:
		explicit FPatchBoundedResponse(int64 InLimit) : Limit(InLimit) {}
		void Serialize(void* Data, int64 Length) override
		{
			FScopeLock Lock(&Mutex);
			if (Length < 0 || Length > Limit - Body.Num())
			{
				bExceeded = true;
				SetError();
				return;
			}
			Body.Append(static_cast<const uint8*>(Data), static_cast<int32>(Length));
		}
		bool TakeBody(TArray<uint8>& Out)
		{
			FScopeLock Lock(&Mutex);
			if (bExceeded) return false;
			Out = MoveTemp(Body);
			return true;
		}
	private:
		FCriticalSection Mutex;
		int64 Limit;
		bool bExceeded = false;
		TArray<uint8> Body;
	};

	bool PatchFetchHttpGet(const FString& Url, double TimeoutSec, int64 MaxBytes, TArray<uint8>& OutBytes,
		FString& OutReason, FString& OutDetail)
	{
#if WITH_DEV_AUTOMATION_TESTS
		if (IsInGameThread() && PatchHttpStub)
		{
			FPatchHttpStubResponse Response = (*PatchHttpStub)(Url);
			if (Response.bTimeout)
			{
				OutReason = TEXT("timeout");
				OutDetail = Url;
				return false;
			}
			FPatchBoundedResponse Body(MaxBytes);
			Body.Serialize(Response.Body.GetData(), Response.Body.Num());
			if (!Body.TakeBody(OutBytes))
			{
				OutReason = TEXT("size");
				OutDetail = Url;
				return false;
			}
			if (Response.Status != 200)
			{
				OutReason = TEXT("http");
				OutDetail = FString::Printf(TEXT("status=%d url=%s"), Response.Status, *Url);
				return false;
			}
			return true;
		}
#endif
		TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Request = FHttpModule::Get().CreateRequest();
		Request->SetURL(Url);
		Request->SetVerb(TEXT("GET"));
		TSharedPtr<bool> bDone = MakeShared<bool>(false);
		TSharedPtr<int32> Status = MakeShared<int32>(0);
		TSharedRef<FPatchBoundedResponse> Body = MakeShared<FPatchBoundedResponse>(MaxBytes);
		if (!Request->SetResponseBodyReceiveStream(Body))
		{
			OutReason = TEXT("http");
			OutDetail = TEXT("stream-unavailable");
			return false;
		}
		Request->OnProcessRequestComplete().BindLambda(
			[bDone, Status](FHttpRequestPtr, FHttpResponsePtr Response, bool bConnected)
			{
				if (bConnected && Response.IsValid())
				{
					*Status = Response->GetResponseCode();
				}
				*bDone = true;
			});
		if (!Request->ProcessRequest())
		{
			OutReason = TEXT("http");
			OutDetail = TEXT("dispatch");
			return false;
		}
		const double Start = FPlatformTime::Seconds();
		while (!*bDone)
		{
			if (FPlatformTime::Seconds() - Start > TimeoutSec)
			{
				Request->CancelRequest();
				OutReason = TEXT("timeout");
				OutDetail = Url;
				return false;
			}
			FHttpModule::Get().GetHttpManager().Tick(0.0f);
			FPlatformProcess::Sleep(0.01f);
		}
		if (!Body->TakeBody(OutBytes))
		{
			OutReason = TEXT("size");
			OutDetail = Url;
			return false;
		}
		if (*Status != 200)
		{
			OutReason = TEXT("http");
			OutDetail = FString::Printf(TEXT("status=%d url=%s"), *Status, *Url);
			return false;
		}
		return true;
	}

	bool PatchFetchReadHttp(const FString& BaseUrl, int64 MaxBytes, int32 TimeoutSec,
		FModKitFetchedPatch& Out, FString& OutReason, FString& OutDetail)
	{
		const double Deadline = FPlatformTime::Seconds() + TimeoutSec;
		const FString Base = BaseUrl.EndsWith(TEXT("/")) ? BaseUrl.LeftChop(1) : BaseUrl;
		TArray<uint8> ManifestBytes;
		if (!PatchFetchHttpGet(Base + TEXT("/Manifest.json"), TimeoutSec, MaxBytes, ManifestBytes, OutReason, OutDetail))
		{
			return false;
		}
		// GetContent() is not null-terminated; convert with an explicit length
		// .
		const FUTF8ToTCHAR ManifestConv(reinterpret_cast<const ANSICHAR*>(ManifestBytes.GetData()),
			ManifestBytes.Num());
		Out.ManifestText = FString(ManifestConv.Length(), ManifestConv.Get());
		// The manifest is parsed here only to enumerate the file list; the full
		// verification chain runs at land time (ModKitHotPatchLand).
		FModKitPatchManifest Manifest;
		FString SchemaReason;
		if (!FModKitPatchManifest::Parse(Out.ManifestText, Manifest, SchemaReason))
		{
			OutReason = TEXT("config");
			OutDetail = TEXT("manifest-enumeration:") + SchemaReason;
			return false;
		}
		int64 Total = ManifestBytes.Num();
		for (const FModKitPatchFileEntry& Entry : Manifest.Files)
		{
			if (Total >= MaxBytes)
			{
				OutReason = TEXT("size");
				return false;
			}
			const double RemainingSeconds = Deadline - FPlatformTime::Seconds();
			if (RemainingSeconds <= 0)
			{
				OutReason = TEXT("timeout");
				OutDetail = TEXT("feed-budget");
				return false;
			}
			TArray<uint8> Bytes;
			if (!PatchFetchHttpGet(Base / Entry.Path, RemainingSeconds, MaxBytes - Total, Bytes, OutReason, OutDetail))
			{
				return false;
			}
			Total += Bytes.Num();
			if (MaxBytes > 0 && Total > MaxBytes)
			{
				OutReason = TEXT("size");
				OutDetail = FString::Printf(TEXT("%lld>%lld"), (long long)Total, (long long)MaxBytes);
				return false;
			}
			Out.Files.Add(Entry.Path, MoveTemp(Bytes));
		}
		Out.TotalBytes = Total;
		return true;
	}
}

bool FModKitHotPatchFetch::Fetch(const FString& FeedUrl, int64 MaxBytes, int32 TimeoutSec,
	FModKitFetchedPatch& Out, FString& OutReason, FString& OutDetail)
{
	Out = FModKitFetchedPatch();
	if (MaxBytes <= 0 || MaxBytes > MAX_int32)
	{
		OutReason = TEXT("config");
		OutDetail = TEXT("invalid-size-limit");
		return false;
	}
	if (FeedUrl.IsEmpty())
	{
		OutReason = TEXT("config");
		OutDetail = TEXT("empty-feed");
		return false;
	}
	if (FeedUrl.StartsWith(TEXT("http://")) || FeedUrl.StartsWith(TEXT("https://")))
	{
		return PatchFetchReadHttp(FeedUrl, MaxBytes, TimeoutSec, Out, OutReason, OutDetail);
	}
	FString LocalPath = FeedUrl;
	LocalPath.RemoveFromStart(TEXT("file://"));
	LocalPath = FPaths::ConvertRelativePathToFull(LocalPath);
	FPaths::NormalizeDirectoryName(LocalPath);
	return PatchFetchReadLocalTree(LocalPath, MaxBytes, Out, OutReason, OutDetail);
}

#if WITH_DEV_AUTOMATION_TESTS
#include "Core/ModKitSettings.h"
#include "HotPatch/ModKitHotPatch.h"
#include "HotPatch/ModKitHotPatchLand.h"
#include "HotPatch/ModKitHotPatchTrigger.h"
#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"

IMPLEMENT_COMPLEX_AUTOMATION_TEST(FModKitHttpStubTest, "AngelscriptModKit.HotPatch.HttpStub",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

void FModKitHttpStubTest::GetTests(TArray<FString>& Names, TArray<FString>& Commands) const
{
	for (const TCHAR* Name : { TEXT("FetchStage"), TEXT("FetchFailure"), TEXT("SignatureRejection"),
		TEXT("HashRejection"), TEXT("PackageRollback"), TEXT("ExplicitApply"), TEXT("ConfiguredFeed") })
	{
		Names.Add(Name);
		Commands.Add(Name);
	}
}

bool FModKitHttpStubTest::RunTest(const FString& Scenario)
{
	if (!TestTrue(TEXT("Stub is scoped to the game thread"), IsInGameThread())
		|| !TestNull(TEXT("No other transport override"), PatchHttpStub)) return false;
	const FString Base = TEXT("http://127.0.0.1/modkit-automation");
	const FString PublicKey = TEXT("bqQ94LWkdv0bTbv7O0yt+TM6JH1qkRZ5fokdmWRoL8s=");
	const FString Root = FPaths::ProjectSavedDir() / TEXT("Automation/ModKitHttpStub") / FGuid::NewGuid().ToString(EGuidFormats::Digits);
	auto* Settings = const_cast<UModKitSettings*>(UModKitSettings::Get());
	const bool WasEnabled = Settings->bEnableHotPatch;
	const FString OldRoot = Settings->PatchStagingRoot;
	const FString OldKey = Settings->PatchPublicKeyBase64;
	const FString OldFeed = Settings->PatchFeedUrl;
	const TArray<FString> OldIds = Settings->PatchAcceptedKeyIds;
	const auto OldLedger = FModKitHotPatch::Ledger;
	Settings->bEnableHotPatch = true;
	Settings->PatchStagingRoot = Root;
	Settings->PatchPublicKeyBase64 = PublicKey;
	Settings->PatchAcceptedKeyIds.AddUnique(TEXT("modkit-g10-sign"));
	ON_SCOPE_EXIT
	{
		PatchHttpStub = nullptr;
		Settings->bEnableHotPatch = WasEnabled;
		Settings->PatchStagingRoot = OldRoot;
		Settings->PatchPublicKeyBase64 = OldKey;
		Settings->PatchFeedUrl = OldFeed;
		Settings->PatchAcceptedKeyIds = OldIds;
		FModKitHotPatch::Ledger = OldLedger;
		IFileManager::Get().DeleteDirectory(*Root, false, true);
	};

	const bool bPackage = Scenario == TEXT("PackageRollback");
	const FString Fixture = FPaths::ProjectDir() / (bPackage
		? TEXT("FeatureModPackages/version-rollback") : TEXT("HotPatch/positive-valid"));
	const FString ManifestPath = Fixture / (bPackage ? TEXT("RollbackFeatureMod.manifest.json") : TEXT("Manifest.json"));
	FString ManifestText, Reason, Detail;
	FModKitPatchManifest Manifest;
	if (!TestTrue(TEXT("Read fixed signed manifest"), FModKitPatchManifest::ReadText(ManifestPath, ManifestText))
		|| !TestTrue(TEXT("Parse fixed signed manifest"), FModKitPatchManifest::Parse(ManifestText, Manifest, Reason))) return false;
	TMap<FString, FPatchHttpStubResponse> Responses;
	FPatchHttpStubResponse ManifestResponse;
	const FTCHARToUTF8 Utf8(*ManifestText);
	ManifestResponse.Body.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	Responses.Add(Base / TEXT("Manifest.json"), MoveTemp(ManifestResponse));
	for (const FModKitPatchFileEntry& Entry : Manifest.Files)
	{
		FPatchHttpStubResponse Response;
		const FString PayloadRoot = bPackage ? Fixture / TEXT("RollbackFeatureMod") : Fixture;
		if (!TestTrue(TEXT("Read fixed signed payload"), FModKitPatchManifest::ReadBytes(PayloadRoot / Entry.Path,
			FModKitPatchManifest::MaxFileBytes, Response.Body))) return false;
		Responses.Add(Base / Entry.Path, MoveTemp(Response));
	}
	TArray<FString> Requests;
	TFunction<FPatchHttpStubResponse(const FString&)> Transport = [&](const FString& Url)
	{
		Requests.Add(Url);
		const FPatchHttpStubResponse* Response = Responses.Find(Url);
		if (!Response) AddError(TEXT("Unexpected HTTP request: ") + Url);
		FPatchHttpStubResponse Missing;
		Missing.Status = 404;
		return Response ? *Response : Missing;
	};
	PatchHttpStub = &Transport;
	FModKitFetchedPatch Patch;
	if (Scenario == TEXT("FetchFailure"))
	{
		IFileManager::Get().MakeDirectory(*(Root / Manifest.PatchId), true);
		const FString Sentinel = Root / Manifest.PatchId / TEXT("old.as");
		TestTrue(TEXT("Prepare old staging"), FFileHelper::SaveStringToFile(TEXT("old"), *Sentinel));
		FModKitHotPatch::Ledger.Add(Manifest.PatchId, EModKitPatchState::Staged);
		Responses.FindChecked(Base / TEXT("Manifest.json")).Status = 503;
		TestFalse(TEXT("503 fails without fallback network"), FModKitHotPatchFetch::Fetch(Base, MAX_int32, 1, Patch, Reason, Detail));
		TestEqual(TEXT("HTTP failure reason"), Reason, FString(TEXT("http")));
		TestTrue(TEXT("HTTP status retained"), Detail.Contains(TEXT("status=503")));
		TestEqual(TEXT("No payload request after manifest failure"), Requests.Num(), 1);
		Responses.FindChecked(Base / TEXT("Manifest.json")).bTimeout = true;
		TestFalse(TEXT("Deterministic timeout"), FModKitHotPatchFetch::Fetch(Base, MAX_int32, 1, Patch, Reason, Detail));
		TestEqual(TEXT("Timeout reason"), Reason, FString(TEXT("timeout")));
		Responses.FindChecked(Base / TEXT("Manifest.json")).bTimeout = false;
		Responses.FindChecked(Base / TEXT("Manifest.json")).Status = 200;
		TestFalse(TEXT("Bounded response rejects oversized manifest"), FModKitHotPatchFetch::Fetch(Base, 1, 1, Patch, Reason, Detail));
		TestEqual(TEXT("Size limit reason"), Reason, FString(TEXT("size")));
		FString Retained;
		TestTrue(TEXT("Old staging remains readable"), FFileHelper::LoadFileToString(Retained, *Sentinel));
		TestEqual(TEXT("Fetch errors preserve old staging"), Retained, FString(TEXT("old")));
		TestEqual(TEXT("Fetch errors preserve ledger"), uint8(FModKitHotPatch::Ledger.FindChecked(Manifest.PatchId)), uint8(EModKitPatchState::Staged));
		return true;
	}
	if (!TestTrue(TEXT("Fetch through local HTTP stub"), FModKitHotPatchFetch::Fetch(Base, MAX_int32, 1, Patch, Reason, Detail))) return false;
	TestEqual(TEXT("Manifest requested first"), Requests[0], Base / TEXT("Manifest.json"));
	TestEqual(TEXT("Only declared files requested"), Requests.Num(), Manifest.Files.Num() + 1);
	TestEqual(TEXT("Manifest byte content preserved"), Patch.ManifestText, ManifestText);
	TestFalse(TEXT("Fetch alone does not land"), IFileManager::Get().DirectoryExists(*Root));
	if (bPackage)
	{
		const FString Floor = Root / TEXT("Floor.json");
		FString Original;
		if (!TestTrue(TEXT("Read higher signed floor"), FModKitPatchManifest::ReadText(Fixture / TEXT("Floor.json"), Original))) return false;
		IFileManager::Get().MakeDirectory(*Root, true);
		TestTrue(TEXT("Install higher floor"), FFileHelper::SaveStringToFile(Original, *Floor, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM));
		TestFalse(TEXT("Old fetched package rejected"), FModKitPatchManifest::VerifyAndCommitPackageFloor(
			Patch.ManifestText, Patch.Files, TEXT("RollbackFeatureMod"), TEXT("1.0"), PublicKey,
			{ TEXT("patch-verify-v1") }, Floor, false, Reason));
		TestEqual(TEXT("Rollback reason"), Reason, FString(TEXT("version-rollback")));
		FString Retained;
		TestTrue(TEXT("Read retained floor"), FModKitPatchManifest::ReadText(Floor, Retained));
		TestEqual(TEXT("Floor unchanged"), Retained, Original);
		return true;
	}

	if (Scenario == TEXT("ConfiguredFeed"))
	{
		Settings->PatchFeedUrl = Base;
		Requests.Reset();
		FModKitHotPatch::RequestFetchApply(FString());
		TestEqual(TEXT("Runtime request uses configured feed"), FModKitHotPatch::PendingFetches.Last(), Base);
		TestFalse(TEXT("Runtime request only enqueues"), IFileManager::Get().DirectoryExists(*Root));
		FTSTicker::GetCoreTicker().RemoveTicker(FModKitHotPatch::TickerHandle);
		FModKitHotPatch::OnTick(0.0f);
		TestEqual(TEXT("Configured feed fetched over stub"), Requests[0], Base / TEXT("Manifest.json"));
		TestEqual(TEXT("Runtime entry only stages"), uint8(FModKitHotPatch::Ledger.FindChecked(Manifest.PatchId)), uint8(EModKitPatchState::Staged));
		Requests.Reset();
		FModKitHotPatch::ExecFetchApply({});
		TestEqual(TEXT("Bare console command uses configured feed"), FModKitHotPatch::PendingFetches.Last(), Base);
		FTSTicker::GetCoreTicker().RemoveTicker(FModKitHotPatch::TickerHandle);
		FModKitHotPatch::OnTick(0.0f);
		TestEqual(TEXT("Console entry only stages"), uint8(FModKitHotPatch::Ledger.FindChecked(Manifest.PatchId)), uint8(EModKitPatchState::Staged));
		TestEqual(TEXT("Console and runtime use the same feed"), Requests[0], Base / TEXT("Manifest.json"));
		return true;
	}
	if (!TestTrue(TEXT("FetchApply stages signed bytes"), FModKitHotPatch::FetchAndLand(Base, PublicKey))) return false;
	TestEqual(TEXT("FetchApply remains Staged"), uint8(FModKitHotPatch::Ledger.FindChecked(Manifest.PatchId)), uint8(EModKitPatchState::Staged));
	FString StagedFile = Root / Manifest.PatchId / Manifest.Files[0].Path;
	FString PreservedPatchId = Manifest.PatchId;
	TArray<uint8> Before;
	TestTrue(TEXT("Staged bytes exist"), FFileHelper::LoadFileToArray(Before, *StagedFile));
	TestTrue(TEXT("Landing preserves exact payload"), Before == Patch.Files.FindChecked(Manifest.Files[0].Path));
	if (Scenario == TEXT("FetchStage")) return true;
	if (Scenario == TEXT("ExplicitApply"))
	{
		bool bDeferred = true;
		AddExpectedError(TEXT("reason=editor"), EAutomationExpectedErrorFlags::Contains, 1);
		TestFalse(TEXT("Explicit Apply retains editor boundary"), FModKitHotPatchTrigger::Apply(Manifest.PatchId, bDeferred));
		TestFalse(TEXT("Rejected apply is not deferred"), bDeferred);
		TestEqual(TEXT("Rejected apply leaves staged ledger"), uint8(FModKitHotPatch::Ledger.FindChecked(Manifest.PatchId)), uint8(EModKitPatchState::Staged));
	}
	else
	{
		const FString Negative = FPaths::ProjectDir() / TEXT("HotPatch") / (Scenario == TEXT("SignatureRejection")
			? TEXT("neg-tamper-signature") : TEXT("neg-tamper-payload"));
		if (!TestTrue(TEXT("Read negative manifest"), FModKitPatchManifest::ReadText(Negative / TEXT("Manifest.json"), Patch.ManifestText))) return false;
		FModKitPatchManifest RejectedManifest;
		if (!TestTrue(TEXT("Parse rejected patch identity"), FModKitPatchManifest::Parse(Patch.ManifestText, RejectedManifest, Reason))) return false;
		PreservedPatchId = RejectedManifest.PatchId;
		StagedFile = Root / PreservedPatchId / Manifest.Files[0].Path;
		TestTrue(TEXT("Prepare the rejected patch's previous staging"), IFileManager::Get().MakeDirectory(*FPaths::GetPath(StagedFile), true));
		TestTrue(TEXT("Preserve previous bytes for the same patch id"), FFileHelper::SaveArrayToFile(Before, *StagedFile));
		FModKitHotPatch::Ledger.Add(PreservedPatchId, EModKitPatchState::Staged);
		if (Scenario == TEXT("HashRejection"))
			TestTrue(TEXT("Read tampered payload"), FFileHelper::LoadFileToArray(Patch.Files.FindChecked(Manifest.Files[0].Path), *(Negative / Manifest.Files[0].Path)));
		// Serve the invalid snapshot through the same HTTP boundary before landing.
		FPatchHttpStubResponse& Response = Responses.FindChecked(Base / TEXT("Manifest.json"));
		const FTCHARToUTF8 NegativeUtf8(*Patch.ManifestText);
		Response.Body.Reset();
		Response.Body.Append(reinterpret_cast<const uint8*>(NegativeUtf8.Get()), NegativeUtf8.Length());
		Responses.FindChecked(Base / Manifest.Files[0].Path).Body = Patch.Files.FindChecked(Manifest.Files[0].Path);
		TestTrue(TEXT("Transport accepts invalid signed content"), FModKitHotPatchFetch::Fetch(Base, MAX_int32, 1, Patch, Reason, Detail));
		FString PatchId;
		int32 Count = 0;
		TestFalse(TEXT("Landing rejects invalid content"), FModKitHotPatchLand::Land(Patch, PublicKey, PatchId, Count, Reason, Detail));
		TestEqual(TEXT("Verification reason"), Reason, FString(Scenario == TEXT("SignatureRejection") ? TEXT("bad-signature") : TEXT("sha-mismatch")));
	}
	TArray<uint8> After;
	TestTrue(TEXT("Read old staging after rejection"), FFileHelper::LoadFileToArray(After, *StagedFile));
	TestTrue(TEXT("Rejection preserves old bytes"), After == Before);
	TestEqual(TEXT("Rejection preserves staged ledger"), uint8(FModKitHotPatch::Ledger.FindChecked(PreservedPatchId)), uint8(EModKitPatchState::Staged));
	return true;
}
#endif
