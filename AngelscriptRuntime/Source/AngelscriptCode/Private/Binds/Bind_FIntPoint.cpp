#include "AngelscriptBinds.h"
#include "AngelscriptManager.h"

#include "Helper_StructType.h"
#include "Helper_ToString.h"

struct FIntPointType : TAngelscriptBaseStructType<FIntPoint>
{
	FString GetAngelscriptTypeName() const override
	{
		return TEXT("FIntPoint");
	}

	void ConstructValue(const FAngelscriptTypeUsage& Usage, void* DestinationPtr) const override
	{
		new(DestinationPtr) FIntPoint(0);
	}

	bool NeedConstruct(const FAngelscriptTypeUsage& Usage) const override { return false; }
	bool NeedDestruct(const FAngelscriptTypeUsage& Usage) const override { return false; }

	bool GetCppForm(const FAngelscriptTypeUsage& Usage, FCppForm& OutCppForm) const override
	{
		OutCppForm.CppType = GetAngelscriptTypeName();
		return true;
	}
};

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FIntPoint(FAngelscriptBinds::EOrder::Early, []
{
	FBindFlags Flags;
	Flags.bPOD = true;

	auto FIntPoint_ = FAngelscriptBinds::ValueClass<FIntPoint>("FIntPoint", Flags);

	FIntPoint_.Constructor("void f(int32 X, int32 Y)", [](FIntPoint* Address, int32 X, int32 Y)
	{
		new(Address) FIntPoint(X, Y);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FIntPoint_, "FIntPoint");

	FIntPoint_.Constructor("void f()", [](FIntPoint* Address)
	{
		new(Address) FIntPoint(0);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR_CUSTOMFORM(FIntPoint_, "FIntPoint", "0");

	FIntPoint_.Constructor("void f(int32 F)", [](FIntPoint* Address, int32 I)
	{
		new(Address) FIntPoint(I);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FIntPoint_, "FIntPoint");

	FIntPoint_.Constructor("void f(const FIntPoint& Other)", [](FIntPoint* Address, const FIntPoint& Other)
	{
		new(Address) FIntPoint(Other);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FIntPoint_, "FIntPoint");

	FIntPoint_.Property("int32 X", &FIntPoint::X);
	FIntPoint_.Property("int32 Y", &FIntPoint::Y);

	FIntPoint_.Method("FIntPoint& opAssign(const FIntPoint& Other)", METHODPR_TRIVIAL(FIntPoint&, FIntPoint, operator=, (const FIntPoint&)));

	FIntPoint_.Method("FIntPoint opAdd(const FIntPoint& Other) const", METHODPR_TRIVIAL(FIntPoint, FIntPoint, operator+, (const FIntPoint&) const));
	FIntPoint_.Method("FIntPoint opSub(const FIntPoint& Other) const", METHODPR_TRIVIAL(FIntPoint, FIntPoint, operator-, (const FIntPoint&) const));
	// Implementing using a lambda since FIntPoint doesn't have operator-() at all (don't want to modify the engine to add it at this point)
	FIntPoint_.Method("FIntPoint opNeg() const", [](FIntPoint* Vec) {return FIntPoint(-Vec->X, -Vec->Y); });

	FIntPoint_.Method("FIntPoint opMul(int32 Scale) const", METHODPR_TRIVIAL(FIntPoint, FIntPoint, operator*, (int32) const));
	FIntPoint_.Method("FIntPoint opDiv(int32 Divisor) const", METHODPR_TRIVIAL(FIntPoint, FIntPoint, operator/, (int32) const));

	FIntPoint_.Method("FIntPoint& opMulAssign(int32 Scale)", METHODPR_TRIVIAL(FIntPoint&, FIntPoint, operator*=, (int32)));
	FIntPoint_.Method("FIntPoint& opDivAssign(int32 Scale)", METHODPR_TRIVIAL(FIntPoint&, FIntPoint, operator/=, (int32)));

	FIntPoint_.Method("FIntPoint opAddAssign(const FIntPoint& Other)", METHODPR_TRIVIAL(FIntPoint&, FIntPoint, operator+=, (const FIntPoint&)));
	FIntPoint_.Method("FIntPoint opSubAssign(const FIntPoint& Other)", METHODPR_TRIVIAL(FIntPoint&, FIntPoint, operator-=, (const FIntPoint&)));

	FIntPoint_.Method("const int32& opIndex(int32 Index) no_discard", METHODPR_TRIVIAL(int32&, FIntPoint, operator[], (const int32)));

	FIntPoint_.Method("bool opEquals(const FIntPoint& Other) const", METHODPR_TRIVIAL(bool, FIntPoint, operator==, (const FIntPoint&) const));

	FIntPoint_.Method("int32 GetMax() const", METHOD_TRIVIAL(FIntPoint, GetMax));
	FIntPoint_.Method("int32 GetMin() const", METHOD_TRIVIAL(FIntPoint, GetMin));
	FIntPoint_.Method("int32 Size() const", METHOD_TRIVIAL(FIntPoint, Size));

	//FIntPoint_.Method("bool IsZero() const", METHOD_TRIVIAL(FIntPoint, IsZero));

	FToStringHelper::Register(TEXT("FIntPoint"), [](void* Ptr, FString& Str)
	{
		Str += ((FIntPoint*)Ptr)->ToString();
	});

	auto NewIntPointType = MakeShared<FIntPointType>();
	FAngelscriptType::Register(NewIntPointType);
});
