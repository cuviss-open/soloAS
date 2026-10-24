#include "AngelscriptBinds.h"
#include "FCpuProfilerTraceScoped.h"

const FAngelscriptBinds::FBind Bind_TraceCPUProfilerEventScoped(FAngelscriptBinds::EOrder::Late, []
{
	auto FCpuProfilerTraceScoped_ = FAngelscriptBinds::ExistingClass("FCpuProfilerTraceScoped");

	FCpuProfilerTraceScoped_.Constructor("void f(const FName& EventID)", [](FCpuProfilerTraceScoped* Address, const FName& EventID)
		{
			new(Address) FCpuProfilerTraceScoped(EventID);
		});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FCpuProfilerTraceScoped_, "FCpuProfilerTraceScoped");
});
