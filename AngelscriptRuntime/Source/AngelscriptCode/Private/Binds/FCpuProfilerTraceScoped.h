#pragma once

#include "CoreMinimal.h"
#include "UObject/Object.h"
#include "FCpuProfilerTraceScoped.generated.h"

USTRUCT(BlueprintType)
struct ANGELSCRIPTCODE_API FCpuProfilerTraceScoped
{
	GENERATED_BODY()

	FCpuProfilerTraceScoped() {}
	
#if CPUPROFILERTRACE_ENABLED
	FCpuProfilerTraceScoped(const FName& EventID)
	{
		FCpuProfilerTrace::OutputBeginDynamicEvent(EventID);
	}
	~FCpuProfilerTraceScoped()
	{
		FCpuProfilerTrace::OutputEndEvent();
	}
#else
	FCpuProfilerTraceScoped(const FName& EventID) {}
	~FCpuProfilerTraceScoped() {}
#endif
};
