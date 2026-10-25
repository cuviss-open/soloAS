#include "Misc/DefaultValueHelper.h"

#include "Engine/NetSerialization.h"

#include "AngelscriptBinds.h"
#include "AngelscriptDocs.h"
#include "AngelscriptManager.h"

                                                                                                   
#include "ClassGenerator/AngelscriptTerminalDestructorAdmission.h"
                                                                                                 
#include "Helper_StructType.h"
#include "Helper_ToString.h"

struct FVectorType : TAngelscriptBaseStructType<FVector>
{
	FString GetAngelscriptTypeName() const override
	{
		return TEXT("FVector");
	}

	void ConstructValue(const FAngelscriptTypeUsage& Usage, void* DestinationPtr) const override
	{
		new(DestinationPtr) FVector(0.f, 0.f, 0.f);
	}

	bool NeedConstruct(const FAngelscriptTypeUsage& Usage) const override { return false; }
	bool NeedDestruct(const FAngelscriptTypeUsage& Usage) const override { return false; }

	bool MatchesProperty(const FAngelscriptTypeUsage& Usage, const FProperty* Property, EPropertyMatchType MatchType) const override
	{
		const FStructProperty* StructProp = CastField<FStructProperty>(Property);
		if (StructProp == nullptr)
			return false;
		if (StructProp->Struct == GetStruct(Usage))
			return true;
		if (StructProp->Struct == FVector_NetQuantize::StaticStruct())
			return true;
		if (StructProp->Struct == FVector_NetQuantize10::StaticStruct())
			return true;
		if (StructProp->Struct == FVector_NetQuantize100::StaticStruct())
			return true;
		if (StructProp->Struct == FVector_NetQuantizeNormal::StaticStruct())
			return true;
		return false;
	}

	bool DefaultValue_UnrealToAngelscript(const FAngelscriptTypeUsage& Usage, const FString& InValue, FString& OutValue) const override
	{
		if (InValue.IsEmpty())
		{
			OutValue = TEXT("FVector()");
			return true;
		}
		FVector Value;
		if (FDefaultValueHelper::ParseVector(InValue, Value))
		{
			OutValue = FString::Printf(TEXT("FVector(%f,%f,%f)"), Value.X, Value.Y, Value.Z);
			return true;
		}
		return false;
	}

	bool DefaultValue_AngelscriptToUnreal(const FAngelscriptTypeUsage& Usage, const FString& CppForm, FString& OutForm) const override
	{
		FString Parameters;
		if(FDefaultValueHelper::Is( CppForm, TEXT("FVector :: ZeroVector") ))
		{
			return true;
		}
		else if(FDefaultValueHelper::Is(CppForm, TEXT("FVector :: UpVector")))
		{
			OutForm = FString::Printf(TEXT("%f,%f,%f"),
				FVector::UpVector.X, FVector::UpVector.Y, FVector::UpVector.Z);
		}
		else if(FDefaultValueHelper::Is(CppForm, TEXT("FVector :: ForwardVector")))
		{
			OutForm = FString::Printf(TEXT("%f,%f,%f"),
				FVector::ForwardVector.X, FVector::ForwardVector.Y, FVector::ForwardVector.Z);
		}
		else if(FDefaultValueHelper::Is(CppForm, TEXT("FVector :: RightVector")))
		{
			OutForm = FString::Printf(TEXT("%f,%f,%f"),
				FVector::RightVector.X, FVector::RightVector.Y, FVector::RightVector.Z);
		}
		else if(FDefaultValueHelper::Is(CppForm, TEXT("FVector :: OneVector")))
		{
			OutForm = FString::Printf(TEXT("%f,%f,%f"),
				FVector::OneVector.X, FVector::OneVector.Y, FVector::OneVector.Z);
		}
		else if( FDefaultValueHelper::GetParameters(CppForm, TEXT("FVector"), Parameters) )
		{
			FVector Vector;
			double Value;
			if (FDefaultValueHelper::ParseVector(Parameters, Vector))
			{
				OutForm = FString::Printf(TEXT("%f,%f,%f"),
					Vector.X, Vector.Y, Vector.Z);
			}
			else if (FDefaultValueHelper::ParseDouble(Parameters, Value))
			{
				OutForm = FString::Printf(TEXT("%f,%f,%f"),
					Value, Value, Value);
			}
		}

		return !OutForm.IsEmpty();
	}

	bool GetCppForm(const FAngelscriptTypeUsage& Usage, FCppForm& OutCppForm) const override
	{
		OutCppForm.CppType = GetAngelscriptTypeName();
		return true;
	}
};

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FVector(FAngelscriptBinds::EOrder::Early, []
{
	FBindFlags Flags;
	Flags.bPOD = true;
	Flags.ExtraFlags |= asOBJ_BASICMATHTYPE;

	auto FVector_ = FAngelscriptBinds::ValueClass<FVector>("FVector", Flags);
	FVector_.SetTypeUserData(TBaseStructure<FVector>::Get());

	FVector_.Constructor("void f(float64 X, float64 Y, float64 Z)", [](FVector* Address, double X, double Y, double Z)
	{
		new(Address) FVector(X, Y, Z);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FVector_, "FVector");
                                                                                                                   
	Cuviss::TerminalAdmission::RegisterVectorValueConstructor(FAngelscriptBinds::GetPreviousBind(), TBaseStructure<FVector>::Get());
                                                                                                                 

	FVector_.Constructor("void f()", [](FVector* Address)
	{
		new(Address) FVector(0.f);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR_CUSTOMFORM(FVector_, "FVector", "0.f");

	FVector_.Constructor("void f(float64 F)", [](FVector* Address, double F)
	{
		new(Address) FVector(F);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FVector_, "FVector");

	FVector_.Constructor("void f(const FVector& Other)", [](FVector* Address, const FVector& Other)
	{
		new(Address) FVector(Other);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FVector_, "FVector");

	FVector_.Property("float64 X", &FVector::X);
	FVector_.Property("float64 Y", &FVector::Y);
	FVector_.Property("float64 Z", &FVector::Z);

	FVector_.Method("FVector& opAssign(const FVector& Other)", METHODPR_TRIVIAL(FVector&, FVector, operator=, (const FVector&)));

	FVector_.Method("FVector opAdd(const FVector& Other) const", METHODPR_TRIVIAL(FVector, FVector, operator+, (const FVector&) const));
	FVector_.Method("FVector opSub(const FVector& Other) const", METHODPR_TRIVIAL(FVector, FVector, operator-, (const FVector&) const));
	FVector_.Method("FVector opMul(const FVector& Other) const", METHODPR_TRIVIAL(FVector, FVector, operator*, (const FVector&) const));
	FVector_.Method("FVector opDiv(const FVector& Other) const", METHODPR_TRIVIAL(FVector, FVector, operator/, (const FVector&) const));

	FVector_.Method("FVector opMul(float64 Scale) const", METHODPR_TRIVIAL(FVector, FVector, operator*, (double) const));
	FVector_.Method("FVector opDiv(float64 Scale) const", METHODPR_TRIVIAL(FVector, FVector, operator/, (double) const));

	FVector_.Method("FVector opNeg() const", METHODPR_TRIVIAL(FVector, FVector, operator-, () const));

	FVector_.Method("FVector opMulAssign(float64 Scale)", METHODPR_TRIVIAL(FVector, FVector, operator*=, (double)));
	FVector_.Method("FVector opDivAssign(float64 Scale)", METHODPR_TRIVIAL(FVector, FVector, operator/=, (double)));

	FVector_.Method("FVector opMulAssign(const FVector& Other)", METHODPR_TRIVIAL(FVector, FVector, operator*=, (const FVector&)));
	FVector_.Method("FVector opDivAssign(const FVector& Other)", METHODPR_TRIVIAL(FVector, FVector, operator/=, (const FVector&)));

	FVector_.Method("FVector opAddAssign(const FVector& Other)", METHODPR_TRIVIAL(FVector, FVector, operator+=, (const FVector&)));
	FVector_.Method("FVector opSubAssign(const FVector& Other)", METHODPR_TRIVIAL(FVector, FVector, operator-=, (const FVector&)));

	FVector_.Method("float64& opIndex(int32 Index) no_discard", METHODPR_TRIVIAL(double&, FVector, operator[], (int32)));
	FVector_.Method("float64 opIndex(int32 Index) const no_discard", METHODPR_TRIVIAL(double, FVector, operator[], (int32) const));

	FVector_.Method("bool opEquals(const FVector& Other) const", METHODPR_TRIVIAL(bool, FVector, operator==, (const FVector&) const));
	FVector_.Method("bool Equals(const FVector& Other, float64 Tolerance = KINDA_SMALL_NUMBER) const", METHOD_TRIVIAL(FVector, Equals));


	FVector_.Method("FVector CrossProduct(const FVector& Other) const", FUNC_TRIVIAL(FVector::CrossProduct));
	FVector_.Method("float64 DotProduct(const FVector& Other) const", FUNC_TRIVIAL(FVector::DotProduct));

	FVector_.Method("bool AllComponentsEqual(float64 Tolerance = KINDA_SMALL_NUMBER) const", METHOD_TRIVIAL(FVector, AllComponentsEqual));

	FVector_.Method("bool Parallel(const FVector& Normal2, float64 ParallelCosineThreshold = THRESH_NORMALS_ARE_PARALLEL) const", FUNC_TRIVIAL(FVector::Parallel));
	SCRIPT_BIND_DOCUMENTATION(
	 "* See if two normal vectors are nearly parallel, meaning the angle between them is close to 0 degrees. \n"
	 "* @param  Normal1 First normalized vector.\n"
	 "* @param  Normal1 Second normalized vector.\n"
	 "* @param  ParallelCosineThreshold Normals are parallel if absolute value of dot product (cosine of angle between them) is greater than or equal to this. For example: cos(1.0 degrees). \n"
	 "* @return true if vectors are nearly parallel, false otherwise. \n"
	)

	FVector_.Method("bool Coincident(const FVector& Normal2, float64 ParallelCosineThreshold = THRESH_NORMALS_ARE_PARALLEL) const", FUNC_TRIVIAL(FVector::Coincident));
	SCRIPT_BIND_DOCUMENTATION(
	 "* See if two normal vectors are coincident (nearly parallel and point in the same direction).\n"
	 "* @param  Normal1 First normalized vector.\n"
	 "* @param  Normal2 Second normalized vector.\n"
	 "* @param  ParallelCosineThreshold Normals are coincident if dot product (cosine of angle between them) is greater than or equal to this. For example: cos(1.0 degrees).\n"
	 "* @return true if vectors are coincident (nearly parallel and point in the same direction), false otherwise.\n"
	)

	FVector_.Method("bool Orthogonal(const FVector& Normal2, float64 OrthogonalCosineThreshold = THRESH_NORMALS_ARE_ORTHOGONAL) const", FUNC_TRIVIAL(FVector::Orthogonal));
	SCRIPT_BIND_DOCUMENTATION(
	 "* See if two normal vectors are nearly orthogonal (perpendicular), meaning the angle between them is close to 90 degrees.\n"
	 "* @param  Normal1 First normalized vector.\n"
	 "* @param  Normal2 Second normalized vector.\n"
	 "* @param  OrthogonalCosineThreshold Normals are orthogonal if absolute value of dot product (cosine of angle between them) is less than or equal to this. For example: cos(89.0 degrees).\n"
	 "* @return true if vectors are orthogonal (perpendicular), false otherwise.\n"
	)

	FVector_.Method("float64 GetMax() const", METHOD_TRIVIAL(FVector, GetMax));
	FVector_.Method("float64 GetAbsMax() const", METHOD_TRIVIAL(FVector, GetAbsMax));
	FVector_.Method("float64 GetMin() const", METHOD_TRIVIAL(FVector, GetMin));
	FVector_.Method("float64 GetAbsMin() const", METHOD_TRIVIAL(FVector, GetAbsMin));

	FVector_.Method("FVector ComponentMin(const FVector& Other) const", METHOD_TRIVIAL(FVector, ComponentMin));
	SCRIPT_BIND_DOCUMENTATION("Gets the component-wise min of two vectors.");

	FVector_.Method("FVector ComponentMax(const FVector& Other) const", METHOD_TRIVIAL(FVector, ComponentMax));
	SCRIPT_BIND_DOCUMENTATION("Gets the component-wise max of two vectors.");

	// This is only available as BoundToBox in C++, but I like the symmetry with ComponentMin/ComponentMax
	FVector_.Method("FVector ComponentClamp(const FVector& Min, const FVector& Max) const", METHOD_TRIVIAL(FVector, BoundToBox));
	SCRIPT_BIND_DOCUMENTATION("Gets the vector clamped component-wise between the specified vectors.");

	FVector_.Method("FVector GetAbs() const", METHOD_TRIVIAL(FVector, GetAbs));
	FVector_.Method("float64 Size() const", METHOD_TRIVIAL(FVector, Size));
	FVector_.Method("float64 SizeSquared() const", METHOD_TRIVIAL(FVector, SizeSquared));
	FVector_.Method("float64 Size2D() const", METHOD_TRIVIAL(FVector, Size2D));
	FVector_.Method("float64 SizeSquared2D() const", METHOD_TRIVIAL(FVector, SizeSquared2D));

	FVector_.Method("bool IsNearlyZero(float64 Tolerance = KINDA_SMALL_NUMBER) const", METHOD_TRIVIAL(FVector, IsNearlyZero));
	FVector_.Method("bool IsZero() const", METHOD_TRIVIAL(FVector, IsZero));

	FVector_.Method("bool Normalize(float64 Tolerance = SMALL_NUMBER)", METHOD_TRIVIAL(FVector, Normalize));
	SCRIPT_BIND_DOCUMENTATION(
		"Normalize this vector in-place if it is larger than a given tolerance. Leaves it unchanged if not.\n"
		"\n"
		"@param Tolerance Minimum squared length of vector for normalization.\n"
		"@return true if the vector was normalized correctly, false otherwise."
	);

	FVector_.Method("bool IsNormalized() const", METHOD_TRIVIAL(FVector, IsNormalized));

	FVector_.Method("void ToDirectionAndLength(FVector& OutDir, float64& OutLength) const", METHODPR_TRIVIAL(void, FVector, ToDirectionAndLength, (FVector&, double&) const));
	SCRIPT_BIND_DOCUMENTATION("Util to convert this vector into a unit direction vector and its original length.");

	FVector_.Method("void ToDirectionAndLength(FVector& OutDir, float32& OutLength) const", METHODPR_TRIVIAL(void, FVector, ToDirectionAndLength, (FVector&, float&) const));
	SCRIPT_BIND_DOCUMENTATION("Util to convert this vector into a unit direction vector and its original length.");

	FVector_.Method("FVector GetSignVector() const", METHOD_TRIVIAL(FVector, GetSignVector));
	SCRIPT_BIND_DOCUMENTATION(
     "Get a copy of the vector as sign only.\n"
     "Each component is set to +1 or -1, with the sign of zero treated as +1.");

	FVector_.Method("FVector Projection() const", METHOD_TRIVIAL(FVector, Projection));
	SCRIPT_BIND_DOCUMENTATION("Projects 2D components of vector based on Z.");

	FVector_.Method("FVector GetUnsafeNormal() const", METHOD_TRIVIAL(FVector, GetUnsafeNormal));
	SCRIPT_BIND_DOCUMENTATION("Calculates normalized version of vector without checking for zero length.");

	FVector_.Method("FVector GridSnap(const float64& GridSize) const", METHOD_TRIVIAL(FVector, GridSnap));

	FVector_.Method("FVector BoundToCube(float64 Radius) const", METHOD_TRIVIAL(FVector, BoundToCube));
	FVector_.Method("FVector BoundToBox(const FVector& Min, const FVector& Max) const", METHOD_TRIVIAL(FVector, BoundToBox));
	FVector_.Method("FVector GetClampedToSize(float64 Min, float64 Max) const", METHOD_TRIVIAL(FVector, GetClampedToSize));
	FVector_.Method("FVector GetClampedToSize2D(float64 Min, float64 Max) const", METHOD_TRIVIAL(FVector, GetClampedToSize2D));
	FVector_.Method("FVector GetClampedToMaxSize(float64 Max) const", METHOD_TRIVIAL(FVector, GetClampedToMaxSize));
	FVector_.Method("FVector GetClampedToMaxSize2D(float64 Max) const", METHOD_TRIVIAL(FVector, GetClampedToMaxSize2D));

	FVector_.Method("void AddBounded(const FVector& V, float64 Radius=MAX_int16)", METHOD_TRIVIAL(FVector, AddBounded));
	SCRIPT_BIND_DOCUMENTATION(
     "Add a vector to this and clamp the result in a cube.\n"
	 "\n"
     "@param V Vector to add.\n"
     "@param Radius Half size of the cube."
	);

	FVector_.Method("FVector Reciprocal() const", METHOD_TRIVIAL(FVector, Reciprocal));
	SCRIPT_BIND_DOCUMENTATION(
     "Gets the reciprocal of this vector, avoiding division by zero.\n"
     "Zero components are set to BIG_NUMBER."
	);

	FVector_.Method("bool IsUniform(float64 Tolerance = KINDA_SMALL_NUMBER) const", METHOD_TRIVIAL(FVector, IsUniform));
	SCRIPT_BIND_DOCUMENTATION(
     "Check whether X, Y and Z are nearly equal.\n"
     "\n"
     "@param Tolerance Specified Tolerance.\n"
     "@return true if X == Y == Z within the specified tolerance."
	);

	FVector_.Method("FVector MirrorByVector(const FVector& MirrorNormal) const", METHOD_TRIVIAL(FVector, MirrorByVector));
	SCRIPT_BIND_DOCUMENTATION("Mirrors a vector about a normal vector");

	FVector_.Method("FVector VectorPlaneProject(const FVector& PlaneNormal) const", FUNC_TRIVIAL(FVector::VectorPlaneProject));
	SCRIPT_BIND_DOCUMENTATION(
     "Calculate the projection of a vector on the plane defined by PlaneNormal.\n"
	 "\n"
     "@param  V The vector to project onto the plane.\n"
     "@param  PlaneNormal Normal of the plane (assumed to be unit length).\n"
     "@return Projection of V onto plane."
	);

	FVector_.Method("FVector RotateAngleAxis(float64 AngleDeg, const FVector& Axis) const", METHOD_TRIVIAL(FVector, RotateAngleAxis));
	SCRIPT_BIND_DOCUMENTATION(
     "Rotates around Axis (assumes Axis.Size() == 1).\n"
     "\n"
     "@param AngleDeg Angle to rotate (in degrees).\n"
     "@param Axis Axis to rotate around.\n"
     "@return Rotated Vector."
	);

	FVector_.Method("FVector RotateAngleAxisRad(float64 AngleRad, const FVector& Axis) const", METHOD_TRIVIAL(FVector, RotateAngleAxisRad));
	SCRIPT_BIND_DOCUMENTATION(
     "Rotates around Axis (assumes Axis.Size() == 1).\n"
     "\n"
     "@param AngleDeg Angle to rotate (in radians).\n"
     "@param Axis Axis to rotate around.\n"
     "@return Rotated Vector."
	);

	FVector_.Method("FVector GetSafeNormal(float64 Tolerance = SMALL_NUMBER, const FVector& ResultIfZero = FVector::ZeroVector) const", METHOD_TRIVIAL(FVector, GetSafeNormal));
	SCRIPT_BIND_DOCUMENTATION(
     "Gets a normalized copy of the vector, checking it is safe to do so based on the length.\n"
     "Returns zero vector by default if vector length is too small to safely normalize.\n"
     "\n"
     "@param Tolerance Minimum squared vector length. Must be positive.\n"
     "@return A normalized copy if safe, ResultIfZero otherwise."
	);

	FVector_.Method("FVector GetSafeNormal2D(float64 Tolerance = SMALL_NUMBER, const FVector& ResultIfZero = FVector::ZeroVector) const", METHOD_TRIVIAL(FVector, GetSafeNormal2D));
	SCRIPT_BIND_DOCUMENTATION(
     "Gets a normalized copy of the 2D components of the vector, checking it is safe to do so. Z is set to zero. \n"
     "Returns zero vector by default if vector length is too small to normalize.\n"
     "\n"
     "@param Tolerance Minimum squared vector length. Must be positive.\n"
     "@return Normalized copy if safe, otherwise returns ResultIfZero."
	);

	FVector_.Method("float64 CosineAngle2D(FVector B) const", METHOD_TRIVIAL(FVector, CosineAngle2D));
	SCRIPT_BIND_DOCUMENTATION(
     "Returns the cosine of the angle between this vector and another projected onto the XY plane (no Z).\n"
     "\n"
     "@param B the other vector to find the 2D cosine of the angle with.\n"
     "@return The cosine."
	);

	FVector_.Method("FVector ProjectOnTo(const FVector& A) const", METHOD_TRIVIAL(FVector, ProjectOnTo));
	SCRIPT_BIND_DOCUMENTATION(
	 "Gets a copy of this vector projected onto the input vector.\n\n"
	 "@param A	Vector to project onto, does not assume it is normalized.\n"
	 "@return Projected vector."
	);

	FVector_.Method("FVector ProjectOnToNormal(const FVector& Normal) const", METHOD_TRIVIAL(FVector, ProjectOnToNormal));
	SCRIPT_BIND_DOCUMENTATION(
	 "Gets a copy of this vector projected onto the input vector, which is assumed to be unit length.\n\n"
	 "@param A	Normal vector to project onto (assumed to be unit length).\n"
	 "@return Projected vector."
	);

	FVector_.Method("void FindBestAxisVectors(FVector& Axis1, FVector& Axis2) const", METHOD_TRIVIAL(FVector, FindBestAxisVectors));
	SCRIPT_BIND_DOCUMENTATION(
     "Find good arbitrary axis vectors to represent U and V axes of a plane,\n"
     "using this vector as the normal of the plane.\n"
     "\n"
     "@param Axis1 Reference to first axis.\n"
     "@param Axis2 Reference to second axis."
	);

	FVector_.Method("void UnwindEuler() const", METHOD_TRIVIAL(FVector, UnwindEuler));
	SCRIPT_BIND_DOCUMENTATION("When this vector contains Euler angles (degrees), ensure that angles are between +/-180");

	FVector_.Method("bool ContainsNaN() const", METHOD_TRIVIAL(FVector, ContainsNaN));
	SCRIPT_BIND_DOCUMENTATION("Utility to check if there are any non-finite values (NaN or Inf) in this vector.");

	FVector_.Method("bool IsUnit(float64 LengthSquaredTolerance = KINDA_SMALL_NUMBER) const", METHOD_TRIVIAL(FVector, IsUnit));
	SCRIPT_BIND_DOCUMENTATION(
     "Check if the vector is of unit length, with specified tolerance.\n"
     "\n"
     "@param LengthSquaredTolerance Tolerance against squared length.\n"
     "@return true if the vector is a unit vector within the specified tolerance."
	);

	FVector_.Method("float64 HeadingAngle() const", METHOD_TRIVIAL(FVector, HeadingAngle));
	SCRIPT_BIND_DOCUMENTATION(
     "Convert a direction vector into a 'heading' angle.\n"
     "\n"
     "@return 'Heading' angle between +/-PI. 0 is pointing down +X."
	);

	FVector_.Method("bool PointsAreSame(const FVector& P2) const", FUNC_TRIVIAL(FVector::PointsAreSame));
	SCRIPT_BIND_DOCUMENTATION(
     "Compare two points and see if they're the same, using a threshold.\n"
     "\n"
     "@return Whether points are the same within a threshold. Uses fast distance approximation (linear per-component distance)."
	);

	FVector_.Method("bool PointsAreNear(const FVector& P2, float64 Dist) const", FUNC_TRIVIAL(FVector::PointsAreNear));
	SCRIPT_BIND_DOCUMENTATION(
     "Compare two points and see if they're within specified distance.\n"
     "\n"
     "@return Whether two points are within the specified distance. Uses fast distance approximation (linear per-component distance).\n"
	);

	FVector_.Method("float64 Distance(const FVector& Other) const", FUNC_TRIVIAL(FVector::Distance));
	FVector_.Method("float64 DistSquared(const FVector& Other) const", FUNC_TRIVIAL(FVector::DistSquared));
	FVector_.Method("float64 Dist2D(const FVector& Other) const", FUNC_TRIVIAL(FVector::Dist2D));
	FVector_.Method("float64 DistXY(const FVector& Other) const", FUNC_TRIVIAL(FVector::DistXY));
	FVector_.Method("float64 DistSquaredXY(const FVector& Other) const", FUNC_TRIVIAL(FVector::DistSquaredXY));
	FVector_.Method("float64 DistSquared2D(const FVector& Other) const", FUNC_TRIVIAL(FVector::DistSquared2D));

	{
		FAngelscriptBinds::FNamespace ns("FVector");

		FAngelscriptBinds::BindGlobalVariable("const FVector ZeroVector", &FVector::ZeroVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::ZeroVector");

		FAngelscriptBinds::BindGlobalVariable("const FVector OneVector", &FVector::OneVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::OneVector");
		
		FAngelscriptBinds::BindGlobalVariable("const FVector UpVector", &FVector::UpVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::UpVector");

		FAngelscriptBinds::BindGlobalVariable("const FVector DownVector", &FVector::DownVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::DownVector");
		
		FAngelscriptBinds::BindGlobalVariable("const FVector ForwardVector", &FVector::ForwardVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::ForwardVector");

		FAngelscriptBinds::BindGlobalVariable("const FVector BackwardVector", &FVector::BackwardVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::BackwardVector");
		
		FAngelscriptBinds::BindGlobalVariable("const FVector RightVector", &FVector::RightVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::RightVector");

		FAngelscriptBinds::BindGlobalVariable("const FVector LeftVector", &FVector::LeftVector);
		SCRIPT_NATIVE_GLOBAL_VARIABLE("FVector::LeftVector");
	}

	FToStringHelper::Register(TEXT("FVector"), [](void* Ptr, FString& Str)
	{
		Str += ((FVector*)Ptr)->ToString();
	});

	auto VectorType = MakeShared<FVectorType>();
	FAngelscriptType::Register(VectorType);
	FAngelscriptType::RegisterTypeFinder([VectorType](FProperty* Property, FAngelscriptTypeUsage& Usage) -> bool
	{
		FStructProperty* StructProp = CastField<FStructProperty>(Property);
		if (StructProp == nullptr)
			return false;

		if (StructProp->Struct == FVector_NetQuantize::StaticStruct()
			|| StructProp->Struct == FVector_NetQuantize10::StaticStruct()
			|| StructProp->Struct == FVector_NetQuantize100::StaticStruct()
			|| StructProp->Struct == FVector_NetQuantizeNormal::StaticStruct())
		{
			Usage.Type = VectorType;
			return true;
		}

		return false;
	});
});

AS_FORCE_LINK const FAngelscriptBinds::FBind Bind_FVector_Conversion(FAngelscriptBinds::EOrder::Late, []
{
	auto FVector_ = FAngelscriptBinds::ExistingClass("FVector");

	FVector_.Method("FRotator ToOrientationRotator() const", METHOD_TRIVIAL(FVector, ToOrientationRotator));
	SCRIPT_BIND_DOCUMENTATION(
     "Return the FRotator orientation corresponding to the direction in which the vector points.\n"
     "Sets Yaw and Pitch to the proper numbers, and sets Roll to zero because the roll can't be determined from a vector.\n"
     "\n"
     "@return FRotator from the Vector's direction, without any roll.\n"
     "@see ToOrientationQuat()"
	);

	FVector_.Method("FQuat ToOrientationQuat() const", METHOD_TRIVIAL(FVector, ToOrientationQuat));
	SCRIPT_BIND_DOCUMENTATION(
     "Return the Quaternion orientation corresponding to the direction in which the vector points.\n"
     "\n"
     "@note If you don't care about preserving the up vector and just want the most direct rotation, you can use the faster\n"
     "'FQuat::FindBetweenVectors(FVector::ForwardVector, YourVector)' or 'FQuat::FindBetweenNormals(...)' if you know the vector is of unit length.\n"
     "\n"
     "@return Quaternion from the Vector's direction, without any roll.\n"
     "@see ToOrientationRotator(), FQuat::FindBetweenVectors()"
	);

	FVector_.Method("FRotator Rotation() const", METHOD_TRIVIAL(FVector, Rotation));
	FAngelscriptBinds::DeprecatePreviousBind("Rotation() is legacy, use ToOrientationRotator() instead to increase readability");

	FVector_.Method("FVector MirrorByPlane(const FPlane& Plane) const", METHOD_TRIVIAL(FVector, MirrorByPlane));
	SCRIPT_BIND_DOCUMENTATION("Mirrors a vector about a plane");

	FVector_.Method("bool InitFromString(const FString& SourceString)", METHOD_TRIVIAL(FVector, InitFromString));
	SCRIPT_BIND_DOCUMENTATION(
     "Initialize this Vector based on an FString. The String is expected to contain X=, Y=, Z=.\n"
     "The TVector<T> will be bogus when InitFromString returns false.\n"
     "\n"
     "@param	InSourceString	FString containing the vector values.\n"
     "@return true if the X,Y,Z values were read successfully; false otherwise.\n"
	);

	FVector_.Constructor("void f(const FVector3f& Other)", [](FVector* Address, const FVector3f& Other)
	{
		new(Address) FVector(Other);
	});
	FAngelscriptBinds::SetPreviousBindNoDiscard(true);
	SCRIPT_TRIVIAL_NATIVE_CONSTRUCTOR(FVector_, "FVector");
});