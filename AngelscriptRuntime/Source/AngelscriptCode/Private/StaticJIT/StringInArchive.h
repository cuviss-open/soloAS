#pragma once
#include "CoreMinimal.h"
#include "Serialization/MemoryReader.h"

static ANSICHAR* GetEmptyStringInArchivePtr()
{
	static ANSICHAR EmptyString[] = "";
	return EmptyString;
}

struct FMemoryReaderWithPtr : public FMemoryReader
{
	const TArray<uint8>& PtrBytes;

	FMemoryReaderWithPtr(const TArray<uint8>& InBytes, bool bIsPersistent = false)
		: FMemoryReader(InBytes, bIsPersistent)
		, PtrBytes(InBytes)
	{
	}

	void* GetCurrentPtr()
	{
		return (void*)&PtrBytes[Tell()];
	}
                                                                                                         
	virtual void Serialize(void* Data, int64 Num) override
	{
		FMemoryReader::Serialize(Data, Num);
		// Container serializers can finish constructing entries after a failed read.
		// Keep unread scalars deterministic until the caller rejects the archive.
		if (IsError() && Num > 0)
			FMemory::Memzero(Data, Num);
	}
                                                                                                       
};

struct FStringInArchive
{
	ANSICHAR* Ptr;
	int32 Length;
	bool bControlsAllocation;

	FStringInArchive()
	{
		Length = 0;
		Ptr = GetEmptyStringInArchivePtr();
		bControlsAllocation = false;
	}

	FStringInArchive(const FStringInArchive& Other)
	{
		bControlsAllocation = false;
		*this = Other;
	}

	FStringInArchive(FStringInArchive&& Other)
	{
		bControlsAllocation = false;
		*this = MoveTemp(Other);
	}

	FStringInArchive(const FString& Str)
	{
		bControlsAllocation = false;
		*this = Str;
	}

	FStringInArchive(const ANSICHAR* Str)
	{
		bControlsAllocation = false;
		*this = Str;
	}

	~FStringInArchive()
	{
		Allocate(0);
	}

	void Allocate(int32 NewLength)
	{
		if (bControlsAllocation)
			FMemory::Free((void*)Ptr);

		if (NewLength == 0)
		{
			Length = 0;
			Ptr = GetEmptyStringInArchivePtr();
			bControlsAllocation = false;
		}
		else
		{
			Ptr = (ANSICHAR*)FMemory::Malloc(sizeof(ANSICHAR) * (NewLength + 1), 8);
			Length = NewLength;
			bControlsAllocation = true;
		}
	}

	int32 Len() const
	{
		return Length;
	}

	void Empty()
	{
		Allocate(0);
	}

	FStringInArchive& operator=(const FStringInArchive& Other)
	{
		if (Other.bControlsAllocation)
		{
			Allocate(Other.Length);
			FMemory::Memcpy(Ptr, Other.Ptr, (Other.Length + 1) * sizeof(ANSICHAR));
		}
		else
		{
			Allocate(0);
			Length = Other.Length;
			Ptr = Other.Ptr;
		}
		return *this;
	}

	FStringInArchive& operator=(FStringInArchive&& Other)
	{
		Allocate(0);

		bControlsAllocation = Other.bControlsAllocation;
		Length = Other.Length;
		Ptr = Other.Ptr;

		Other.Length = 0;
		Other.bControlsAllocation = false;
		Other.Ptr = GetEmptyStringInArchivePtr();
		return *this;
	}

	FStringInArchive& operator=(const ANSICHAR* Str)
	{
		int32 NewLength = FCStringAnsi::Strlen(Str);
		Allocate(NewLength);
		FMemory::Memcpy(Ptr, Str, (NewLength + 1) * sizeof(ANSICHAR));
		return *this;
	}

	FStringInArchive& operator=(const FString& Str)
	{
		*this = TCHAR_TO_ANSI(*Str);
		return *this;
	}

	bool operator==(const ANSICHAR* Other)
	{
		return FCStringAnsi::Strcmp(Ptr, Other) == 0;
	}

	const ANSICHAR* operator*() const
	{
		return Ptr;
	}

	FString UnrealString() const
	{
		return FString(ANSI_TO_TCHAR(Ptr));
	}

	void AssignAsUTF8(const FString& Other)
	{
		*this = TCHAR_TO_UTF8(*Other);
}

	FString UnrealString_UTF8() const
	{
		return FString(UTF8_TO_TCHAR(Ptr));
	}

	inline friend FArchive& operator<<(FArchive& Ar, FStringInArchive& Data)
	{
		if (Ar.IsSaving())
		{
			Ar << Data.Length;
			if (Data.Length != 0)
				Ar.Serialize(Data.Ptr, (Data.Length + 1) * sizeof(ANSICHAR));
		}
		else
		{
			Ar << Data.Length;
                                                                                                     
			const int64 Position = Ar.Tell();
			const int64 Size = Ar.TotalSize();
			if (Ar.IsError() || Data.Length < 0 || Position < 0 || Position > Size
				|| (Data.Length > 0 && int64(Data.Length) + 1 > Size - Position))
			{
				Data.Empty();
				Ar.SetError();
				return Ar;
			}
			if (Data.Length > 0
				&& ((const ANSICHAR*)((FMemoryReaderWithPtr*)&Ar)->GetCurrentPtr())[Data.Length] != '\0')
			{
				Data.Empty();
				Ar.SetError();
				return Ar;
			}
                                                                                                   
			if (Data.Length != 0)
			{
				Data.Ptr = (ANSICHAR*)((FMemoryReaderWithPtr*)&Ar)->GetCurrentPtr();
				Data.bControlsAllocation = false;
				Ar.Seek(Ar.Tell() + (Data.Length + 1));
			}
			else
			{
				Data.Ptr = GetEmptyStringInArchivePtr();
				Data.bControlsAllocation = false;
			}
		}
		return Ar;
	}
};
