// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
#pragma once

class FModKitScriptExecutionTimeout
{
public:
	static void Startup();
	static void Shutdown();
	static bool IsRetained();
};
