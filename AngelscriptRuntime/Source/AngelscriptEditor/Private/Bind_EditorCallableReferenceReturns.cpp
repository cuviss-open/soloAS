// SPDX-License-Identifier: MIT
// Copyright Cuviss Studio. All Rights Reserved.
#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"
#include "EditorMenuExtensions/ScriptAssetMenuExtension.h"

AS_FORCE_LINK const FAngelscriptBinds::FBind
    Bind_EditorCallableReferenceReturns(FAngelscriptBinds::EOrder::Late, []
{
    if (!FAngelscriptManager::bUseEditorScripts)
    {
        return;
    }

    auto ExtensionBinds =
        FAngelscriptBinds::ExistingClass("UScriptAssetMenuExtension");
    ExtensionBinds.Method(
        "const TArray<FAssetData>& GetSelectedAssets() const",
        [](const UScriptAssetMenuExtension* Extension)
            -> const TArray<FAssetData>&
        {
            return Extension->GetSelectedAssets();
        }
    );
});
