// SPDX-License-Identifier: MPL-2.0
// Copyright Cuviss Studio. All Rights Reserved.
// Cuviss-owned hot-patch signature verification implementation .
//
// Fallback path (red gate 8): the UE-AS fork 5.7 fork has no Crypto module /
// FEncryptionContext, and its Runtime/RSA module FRSA::DecryptPublic is not
// implemented (returns -1 for every input; 2x2 key/signature endianness
// matrix probe in run pf-modkit-mixed-compile-v1-20260904a-UE-AS fork) -> the
// primitive is a self-contained Ed25519 verify-only. The group/scalar/SHA-512
// code below is derived from TweetNaCl 20140427 (https://tweetnacl.cr.yp.to),
// which is in the public domain. SHA-512 (crypto_hash) is self-contained
// because the locked UE-AS fork 5.7 Core has no FSHA512 (same precedent as the
// self-contained SHA-256 in ModKitCacheCipher.h). The product holds no
// private-key material: only the 32-byte public point is configured and the
// verifier performs only the RFC 8032 public-key equation.

#include "Shared/Crypto/ModKitPatchSignature.h"

#include "Misc/Base64.h"

namespace
{
	using PatchSigFe = int64[16];

	// TweetNaCl field/group constants (public domain).
	constexpr PatchSigFe PatchSigFeZero = {};
	constexpr PatchSigFe PatchSigFeOne = { 1 };
	constexpr PatchSigFe PatchSigFeD = { 0x78a3, 0x1359, 0x4dca, 0x75eb, 0xd8ab, 0x4141,
		0x0a4d, 0x0070, 0xe898, 0x7779, 0x4079, 0x8cc7, 0xfe73, 0x2b6f, 0x6cee, 0x5203 };
	constexpr PatchSigFe PatchSigFeD2 = { 0xf159, 0x26b2, 0x9b94, 0xebd6, 0xb156, 0x8283,
		0x149a, 0x00e0, 0xd130, 0xeef3, 0x80f2, 0x198e, 0xfce7, 0x56df, 0xd9dc, 0x2406 };
	constexpr PatchSigFe PatchSigFeX = { 0xd51a, 0x8f25, 0x2d60, 0xc956, 0xa7b2, 0x9525,
		0xc760, 0x692c, 0xdc5c, 0xfdd6, 0xe231, 0xc0a4, 0x53fe, 0xcd6e, 0x36d3, 0x2169 };
	constexpr PatchSigFe PatchSigFeY = { 0x6658, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666,
		0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666, 0x6666 };
	constexpr PatchSigFe PatchSigFeI = { 0xa0b0, 0x4a0e, 0x1b27, 0xc4ee, 0xe478, 0xad2f,
		0x1806, 0x2f43, 0xd7a7, 0x3dfb, 0x0099, 0x2b4d, 0xdf0b, 0x4fc1, 0x2480, 0x2b83 };

	// Ed25519 group order L (little-endian).
	constexpr uint8 PatchSigScalarL[32] = { 0xed, 0xd3, 0xf5, 0x5c, 0x1a, 0x63, 0x12, 0x58,
		0xd6, 0x9c, 0xf7, 0xa2, 0xde, 0xf9, 0xde, 0x14, 0, 0, 0, 0, 0, 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0x10 };

	// ---- field arithmetic (mod 2^255 - 19) ----------------------------------
	void PatchSigFeSet(int64* O, const int64* A)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] = A[I];
		}
	}

	void PatchSigFeCarry(int64* O)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] += (int64)1 << 16;
			const int64 C = O[I] >> 16;
			O[(I + 1) * (I < 15 ? 1 : 0)] += C - 1 + 37 * (C - 1) * (I == 15 ? 1 : 0);
			O[I] -= C << 16;
		}
	}

	void PatchSigFeSelect(int64* P, int64* Q, int32 B)
	{
		const int64 C = ~(B - 1);
		for (int32 I = 0; I < 16; ++I)
		{
			const int64 T = C & (P[I] ^ Q[I]);
			P[I] ^= T;
			Q[I] ^= T;
		}
	}

	void PatchSigFePack(uint8* O, const int64* N)
	{
		int64 T[16], M[16];
		PatchSigFeSet(T, N);
		PatchSigFeCarry(T);
		PatchSigFeCarry(T);
		PatchSigFeCarry(T);
		for (int32 J = 0; J < 2; ++J)
		{
			M[0] = T[0] - 0xffed;
			for (int32 I = 1; I < 15; ++I)
			{
				M[I] = T[I] - 0xffff - ((M[I - 1] >> 16) & 1);
				M[I - 1] &= 0xffff;
			}
			M[15] = T[15] - 0x7fff - ((M[14] >> 16) & 1);
			const int32 B = (int32)((M[15] >> 16) & 1);
			M[14] &= 0xffff;
			PatchSigFeSelect(T, M, 1 - B);
		}
		for (int32 I = 0; I < 16; ++I)
		{
			O[2 * I] = (uint8)(T[I] & 0xff);
			O[2 * I + 1] = (uint8)(T[I] >> 8);
		}
	}

	void PatchSigFeUnpack(int64* O, const uint8* N)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] = (int64)N[2 * I] + ((int64)N[2 * I + 1] << 8);
		}
		O[15] &= 0x7fff;
	}

	void PatchSigFeAdd(int64* O, const int64* A, const int64* B)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] = A[I] + B[I];
		}
	}

	void PatchSigFeSub(int64* O, const int64* A, const int64* B)
	{
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] = A[I] - B[I];
		}
	}

	void PatchSigFeMul(int64* O, const int64* A, const int64* B)
	{
		int64 T[31] = {};
		for (int32 I = 0; I < 16; ++I)
		{
			for (int32 J = 0; J < 16; ++J)
			{
				T[I + J] += A[I] * B[J];
			}
		}
		for (int32 I = 0; I < 15; ++I)
		{
			T[I] += 38 * T[I + 16];
		}
		for (int32 I = 0; I < 16; ++I)
		{
			O[I] = T[I];
		}
		PatchSigFeCarry(O);
		PatchSigFeCarry(O);
	}

	void PatchSigFeSq(int64* O, const int64* A)
	{
		PatchSigFeMul(O, A, A);
	}

	void PatchSigFeInvert(int64* O, const int64* I)
	{
		int64 C[16];
		PatchSigFeSet(C, I);
		for (int32 A = 253; A >= 0; --A)
		{
			PatchSigFeSq(C, C);
			if (A != 2 && A != 4)
			{
				PatchSigFeMul(C, C, I);
			}
		}
		PatchSigFeSet(O, C);
	}

	void PatchSigFePow2523(int64* O, const int64* I)
	{
		int64 C[16];
		PatchSigFeSet(C, I);
		for (int32 A = 250; A >= 0; --A)
		{
			PatchSigFeSq(C, C);
			if (A != 1)
			{
				PatchSigFeMul(C, C, I);
			}
		}
		PatchSigFeSet(O, C);
	}

	// Constant-time 32-byte comparison; 0 when equal (TweetNaCl vn).
	int32 PatchSigVerify32(const uint8* X, const uint8* Y)
	{
		uint32 D = 0;
		for (int32 I = 0; I < 32; ++I)
		{
			D |= X[I] ^ Y[I];
		}
		return (1 & ((D - 1) >> 8)) - 1;
	}

	uint8 PatchSigFePar(const int64* A)
	{
		uint8 D[32];
		PatchSigFePack(D, A);
		return D[0] & 1;
	}

	bool PatchSigFeNeq(const int64* A, const int64* B)
	{
		uint8 C[32], D[32];
		PatchSigFePack(C, A);
		PatchSigFePack(D, B);
		return PatchSigVerify32(C, D) != 0;
	}

	// ---- group arithmetic (extended homogeneous coordinates) ----------------
	void PatchSigGeAdd(PatchSigFe P[4], const PatchSigFe Q[4])
	{
		PatchSigFe A, B, C, D, T, E, F, G, H;
		PatchSigFeSub(A, P[1], P[0]);
		PatchSigFeSub(T, Q[1], Q[0]);
		PatchSigFeMul(A, A, T);
		PatchSigFeAdd(B, P[0], P[1]);
		PatchSigFeAdd(T, Q[0], Q[1]);
		PatchSigFeMul(B, B, T);
		PatchSigFeMul(C, P[3], Q[3]);
		PatchSigFeMul(C, C, PatchSigFeD2);
		PatchSigFeMul(D, P[2], Q[2]);
		PatchSigFeAdd(D, D, D);
		PatchSigFeSub(E, B, A);
		PatchSigFeSub(F, D, C);
		PatchSigFeAdd(G, D, C);
		PatchSigFeAdd(H, B, A);
		PatchSigFeMul(P[0], E, F);
		PatchSigFeMul(P[1], H, G);
		PatchSigFeMul(P[2], G, F);
		PatchSigFeMul(P[3], E, H);
	}

	void PatchSigGeCswap(PatchSigFe P[4], PatchSigFe Q[4], uint8 B)
	{
		for (int32 I = 0; I < 4; ++I)
		{
			PatchSigFeSelect(P[I], Q[I], B);
		}
	}

	void PatchSigGePack(uint8* R, const PatchSigFe P[4])
	{
		PatchSigFe Zi, Tx, Ty;
		PatchSigFeInvert(Zi, P[2]);
		PatchSigFeMul(Tx, P[0], Zi);
		PatchSigFeMul(Ty, P[1], Zi);
		PatchSigFePack(R, Ty);
		R[31] ^= (uint8)(PatchSigFePar(Tx) << 7);
	}

	void PatchSigGeScalarmult(PatchSigFe P[4], const PatchSigFe Q[4], const uint8* S)
	{
		PatchSigFe QMut[4];
		for (int32 I = 0; I < 4; ++I)
		{
			for (int32 J = 0; J < 16; ++J)
			{
				QMut[I][J] = Q[I][J];
			}
		}
		PatchSigFeSet(P[0], PatchSigFeZero);
		PatchSigFeSet(P[1], PatchSigFeOne);
		PatchSigFeSet(P[2], PatchSigFeOne);
		PatchSigFeSet(P[3], PatchSigFeZero);
		for (int32 I = 255; I >= 0; --I)
		{
			const uint8 B = (uint8)((S[I / 8] >> (I & 7)) & 1);
			PatchSigGeCswap(P, QMut, B);
			PatchSigGeAdd(QMut, P);
			PatchSigGeAdd(P, P);
			PatchSigGeCswap(P, QMut, B);
		}
	}

	void PatchSigGeScalarbase(PatchSigFe P[4], const uint8* S)
	{
		PatchSigFe Q[4];
		PatchSigFeSet(Q[0], PatchSigFeX);
		PatchSigFeSet(Q[1], PatchSigFeY);
		PatchSigFeSet(Q[2], PatchSigFeOne);
		PatchSigFeMul(Q[3], PatchSigFeX, PatchSigFeY);
		PatchSigGeScalarmult(P, Q, S);
	}

	// Decodes the public point and returns it negated; -1 on non-canonical input.
	int32 PatchSigGeUnpackNeg(PatchSigFe R[4], const uint8* Pk)
	{
		PatchSigFe T, Chk, Num, Den, Den2, Den4, Den6;
		PatchSigFeSet(R[2], PatchSigFeOne);
		PatchSigFeUnpack(R[1], Pk);
		PatchSigFeSq(Num, R[1]);
		PatchSigFeMul(Den, Num, PatchSigFeD);
		PatchSigFeSub(Num, Num, R[2]);
		PatchSigFeAdd(Den, R[2], Den);
		PatchSigFeSq(Den2, Den);
		PatchSigFeSq(Den4, Den2);
		PatchSigFeMul(Den6, Den4, Den2);
		PatchSigFeMul(T, Den6, Num);
		PatchSigFeMul(T, T, Den);
		PatchSigFePow2523(T, T);
		PatchSigFeMul(T, T, Num);
		PatchSigFeMul(T, T, Den);
		PatchSigFeMul(T, T, Den);
		PatchSigFeMul(R[0], T, Den);
		PatchSigFeSq(Chk, R[0]);
		PatchSigFeMul(Chk, Chk, Den);
		if (PatchSigFeNeq(Chk, Num))
		{
			PatchSigFeMul(R[0], R[0], PatchSigFeI);
		}
		PatchSigFeSq(Chk, R[0]);
		PatchSigFeMul(Chk, Chk, Den);
		if (PatchSigFeNeq(Chk, Num))
		{
			return -1;
		}
		uint8 Parity = PatchSigFePar(R[0]);
		if (Parity == (Pk[31] >> 7))
		{
			PatchSigFeSub(R[0], PatchSigFeZero, R[0]);
		}
		PatchSigFeMul(R[3], R[0], R[1]);
		return 0;
	}

	// ---- scalar arithmetic (mod L) ------------------------------------------
	void PatchSigModL(uint8* R, int64* X)
	{
		int64 Carry;
		int32 J;
		for (int32 I = 63; I >= 32; --I)
		{
			Carry = 0;
			for (J = I - 32; J < I - 12; ++J)
			{
				X[J] += Carry - 16 * X[I] * PatchSigScalarL[J - (I - 32)];
				Carry = (X[J] + 128) >> 8;
				X[J] -= Carry << 8;
			}
			X[J] += Carry;
			X[I] = 0;
		}
		Carry = 0;
		for (J = 0; J < 32; ++J)
		{
			X[J] += Carry - (X[31] >> 4) * PatchSigScalarL[J];
			Carry = X[J] >> 8;
			X[J] &= 255;
		}
		for (J = 0; J < 32; ++J)
		{
			X[J] -= Carry * PatchSigScalarL[J];
		}
		for (int32 I = 0; I < 32; ++I)
		{
			X[I + 1] += X[I] >> 8;
			R[I] = (uint8)(X[I] & 255);
		}
	}

	// Reduces the 64-byte little-endian value in R[0..63] and stores the
	// 32-byte result in R[0..31] (R[32..63] is zeroed).
	void PatchSigReduce(uint8* R)
	{
		int64 X[64];
		for (int32 I = 0; I < 64; ++I)
		{
			X[I] = (int64)R[I];
		}
		for (int32 I = 0; I < 64; ++I)
		{
			R[I] = 0;
		}
		PatchSigModL(R, X);
	}

	// ---- SHA-512 (crypto_hash, self-contained) ------------------------------
	uint64 PatchSigLoad64(const uint8* X)
	{
		uint64 U = 0;
		for (int32 I = 0; I < 8; ++I)
		{
			U = (U << 8) | X[I];
		}
		return U;
	}

	void PatchSigStore64(uint8* X, uint64 U)
	{
		for (int32 I = 7; I >= 0; --I)
		{
			X[I] = (uint8)U;
			U >>= 8;
		}
	}

	uint64 PatchSigRotr(uint64 X, int32 C)
	{
		return (X >> C) | (X << (64 - C));
	}

	uint64 PatchSigCh(uint64 X, uint64 Y, uint64 Z)
	{
		return (X & Y) ^ (~X & Z);
	}

	uint64 PatchSigMaj(uint64 X, uint64 Y, uint64 Z)
	{
		return (X & Y) ^ (X & Z) ^ (Y & Z);
	}

	uint64 PatchSigSigma0(uint64 X)
	{
		return PatchSigRotr(X, 28) ^ PatchSigRotr(X, 34) ^ PatchSigRotr(X, 39);
	}

	uint64 PatchSigSigma1(uint64 X)
	{
		return PatchSigRotr(X, 14) ^ PatchSigRotr(X, 18) ^ PatchSigRotr(X, 41);
	}

	uint64 PatchSigSig0(uint64 X)
	{
		return PatchSigRotr(X, 1) ^ PatchSigRotr(X, 8) ^ (X >> 7);
	}

	uint64 PatchSigSig1(uint64 X)
	{
		return PatchSigRotr(X, 19) ^ PatchSigRotr(X, 61) ^ (X >> 6);
	}

	constexpr uint64 PatchSigSha512K[80] = {
		0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL, 0xe9b5dba58189dbbcULL,
		0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL, 0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL,
		0xd807aa98a3030242ULL, 0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e2ULL,
		0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL, 0xc19bf174cf692694ULL,
		0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL, 0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL,
		0x2de92c6f592b0275ULL, 0x4a7484aa6ea6e483ULL, 0x5cb0a9dcbd41fbd4ULL, 0x76f988da831153b5ULL,
		0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL, 0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL,
		0xc6e00bf33da88fc2ULL, 0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
		0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL, 0x53380d139d95b3dfULL,
		0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL, 0x81c2c92e47edaee6ULL, 0x92722c851482353bULL,
		0xa2bfe8a14cf10364ULL, 0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
		0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e35855771202aULL, 0x106aa07032bbd1b8ULL,
		0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL, 0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL,
		0x391c0cb3c5c95a63ULL, 0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b2b8a3ULL,
		0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL, 0x8cc702081a6439ecULL,
		0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL, 0xbef9a3f7b2c67915ULL, 0xc67178f2e372532bULL,
		0xca273eceea26619cULL, 0xd186b8c721c0c207ULL, 0xeada7dd6cde0eb1eULL, 0xf57d4f7fee6ed178ULL,
		0x06f067aa72176fbaULL, 0x0a637dc5a2c898a6ULL, 0x113f9804bef90daeULL, 0x1b710b35131c471bULL,
		0x28db77f523047d84ULL, 0x32caab7b40c72493ULL, 0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL,
		0x4cc5d4becb3e42b6ULL, 0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL };

	constexpr uint8 PatchSigSha512Iv[64] = {
		0x6a, 0x09, 0xe6, 0x67, 0xf3, 0xbc, 0xc9, 0x08,
		0xbb, 0x67, 0xae, 0x85, 0x84, 0xca, 0xa7, 0x3b,
		0x3c, 0x6e, 0xf3, 0x72, 0xfe, 0x94, 0xf8, 0x2b,
		0xa5, 0x4f, 0xf5, 0x3a, 0x5f, 0x1d, 0x36, 0xf1,
		0x51, 0x0e, 0x52, 0x7f, 0xad, 0xe6, 0x82, 0xd1,
		0x9b, 0x05, 0x68, 0x8c, 0x2b, 0x3e, 0x6c, 0x1f,
		0x1f, 0x83, 0xd9, 0xab, 0xfb, 0x41, 0xbd, 0x6b,
		0x5b, 0xe0, 0xcd, 0x19, 0x13, 0x7e, 0x21, 0x79 };

	int32 PatchSigHashBlock(uint8* X, const uint8* M, uint64 N)
	{
		uint64 Z[8], B[8], A[8], W[16], T;
		for (int32 I = 0; I < 8; ++I)
		{
			Z[I] = A[I] = PatchSigLoad64(X + 8 * I);
		}
		while (N >= 128)
		{
			for (int32 I = 0; I < 16; ++I)
			{
				W[I] = PatchSigLoad64(M + 8 * I);
			}
			for (int32 I = 0; I < 80; ++I)
			{
				for (int32 J = 0; J < 8; ++J)
				{
					B[J] = A[J];
				}
				T = A[7] + PatchSigSigma1(A[4]) + PatchSigCh(A[4], A[5], A[6]) + PatchSigSha512K[I] + W[I % 16];
				B[7] = T + PatchSigSigma0(A[0]) + PatchSigMaj(A[0], A[1], A[2]);
				B[3] += T;
				for (int32 J = 0; J < 8; ++J)
				{
					A[(J + 1) % 8] = B[J];
				}
				if (I % 16 == 15)
				{
					for (int32 J = 0; J < 16; ++J)
					{
						W[J] += W[(J + 9) % 16] + PatchSigSig0(W[(J + 1) % 16]) + PatchSigSig1(W[(J + 14) % 16]);
					}
				}
			}
			for (int32 I = 0; I < 8; ++I)
			{
				A[I] += Z[I];
				Z[I] = A[I];
			}
			M += 128;
			N -= 128;
		}
		for (int32 I = 0; I < 8; ++I)
		{
			PatchSigStore64(X + 8 * I, Z[I]);
		}
		return (int32)N;
	}

	void PatchSigSha512(uint8* Out, const uint8* M, uint64 N)
	{
		uint8 H[64], X[256];
		uint64 B = N;
		for (int32 I = 0; I < 64; ++I)
		{
			H[I] = PatchSigSha512Iv[I];
		}
		PatchSigHashBlock(H, M, N);
		M += N;
		N &= 127;
		M -= N;
		for (int32 I = 0; I < 256; ++I)
		{
			X[I] = 0;
		}
		for (uint64 I = 0; I < N; ++I)
		{
			X[I] = M[I];
		}
		X[N] = 128;
		N = 256 - 128 * (N < 112 ? 1 : 0);
		X[N - 9] = (uint8)(B >> 61);
		PatchSigStore64(X + N - 8, B << 3);
		PatchSigHashBlock(H, X, N);
		for (int32 I = 0; I < 64; ++I)
		{
			Out[I] = H[I];
		}
	}
}

bool FModKitPatchSignature::DecodePublicKey(const FString& Base64, FPublicKey& OutKey)
{
	TArray<uint8> Bytes;
	if (!FBase64::Decode(Base64, Bytes) || Bytes.Num() != 32)
	{
		return false;
	}
	FMemory::Memcpy(OutKey.Bytes, Bytes.GetData(), 32);
	return true;
}

bool FModKitPatchSignature::Verify(const TArray<uint8>& CanonicalPayload, const TArray<uint8>& SignatureBytes,
	const FPublicKey& Key)
{
	if (SignatureBytes.Num() != 64)
	{
		return false;
	}
	const uint8* Sig = SignatureBytes.GetData();

	// RFC 8032 canonicality gate: reject S >= L. S mod L != S exactly when S >= L.
	uint8 ScalarProbe[64] = {};
	FMemory::Memcpy(ScalarProbe, Sig + 32, 32);
	PatchSigReduce(ScalarProbe);
	if (FMemory::Memcmp(ScalarProbe, Sig + 32, 32) != 0)
	{
		return false;
	}

	// Decode the public point (negated): -A; fail closed on non-canonical keys.
	PatchSigFe ANeg[4];
	if (PatchSigGeUnpackNeg(ANeg, Key.Bytes) != 0)
	{
		return false;
	}

	// H = SHA-512(R || A || M) reduced mod L.
	const int32 PayloadBytes = CanonicalPayload.Num();
	TArray<uint8> HashInput;
	HashInput.SetNum(64 + PayloadBytes);
	FMemory::Memcpy(HashInput.GetData(), Sig, 32);
	FMemory::Memcpy(HashInput.GetData() + 32, Key.Bytes, 32);
	if (PayloadBytes > 0)
	{
		FMemory::Memcpy(HashInput.GetData() + 64, CanonicalPayload.GetData(), PayloadBytes);
	}
	uint8 Hash[64];
	PatchSigSha512(Hash, HashInput.GetData(), 64 + PayloadBytes);
	PatchSigReduce(Hash);

	// R' = [h](-A) + [S]B; accept iff encode(R') == R.
	PatchSigFe P[4], Q[4];
	PatchSigGeScalarmult(P, ANeg, Hash);
	PatchSigGeScalarbase(Q, Sig + 32);
	PatchSigGeAdd(P, Q);
	uint8 ExpectedR[32];
	PatchSigGePack(ExpectedR, P);
	return PatchSigVerify32(Sig, ExpectedR) == 0;
}