#!/usr/bin/env python3
"""Generates the C edge-case checks. Every expected value is computed HERE, independently of the guest."""
import struct, hashlib, math

M32 = (1 << 32) - 1; M64 = (1 << 64) - 1
def s32(x): x &= M32; return x - (1 << 32) if x >> 31 else x
def u32(x): return x & M32
def s64(x): x &= M64; return x - (1 << 64) if x >> 63 else x
def u64(x): return x & M64
def tdiv(a, b):
    q = abs(a) // abs(b); return q if (a < 0) == (b < 0) else -q
def trem(a, b): return a - b * tdiv(a, b)
def rotl(x, n, w):
    n %= w; m = (1 << w) - 1
    return ((x << n) | (x >> (w - n))) & m if n else x
def rotr(x, n, w): return rotl(x, w - (n % w), w)
def clz(x, w): return w - x.bit_length()
def ctz(x, w): return w if x == 0 else (x & -x).bit_length() - 1
def pop(x): return bin(x).count('1')

def l32(v):  return "(-2147483647-1)" if v == -2**31 else (f"({v})" if v < 0 else str(v))
def lu32(v): return f"{v}u"
def l64(v):  return "(-9223372036854775807ll-1)" if v == -2**63 else (f"({v}ll)" if v < 0 else f"{v}ll")
def lu64(v): return f"{v}ull"

out_i = []   # int module generated checks
def emit(buf, kind, name, cexpr, exp):
    lit = {'i32': l32, 'u32': lu32, 'i64': l64, 'u64': lu64}[kind](exp)
    buf.append(f'    chk_{kind}("{name}", {cexpr}, {lit});')

# ------------------------------------------------------------------ i32
I = out_i
def sec(buf, title): buf.append(f'    ew_println("-- {title}");')

sec(I, "i32 wraparound")
emit(I,'i32',"INT_MAX + 1","(int32_t)(vu32(0x7FFFFFFFu) + vu32(1u))", s32(2**31-1+1))
emit(I,'i32',"INT_MIN - 1","(int32_t)(vu32(0x80000000u) - vu32(1u))", s32(-2**31-1))
emit(I,'i32',"0 - INT_MIN","(int32_t)(vu32(0u) - vu32(0x80000000u))", s32(2**31))
emit(I,'i32',"INT_MIN * -1","(int32_t)(vu32(0x80000000u) * vu32(0xFFFFFFFFu))", s32(2**31))
emit(I,'i32',"65536 * 65536","(int32_t)(vu32(65536u) * vu32(65536u))", 0)
emit(I,'i32',"46341 * 46341","(int32_t)(vu32(46341u) * vu32(46341u))", s32(46341*46341))
emit(I,'i32',"65537 * 65537","(int32_t)(vu32(65537u) * vu32(65537u))", s32(65537*65537))
emit(I,'u32',"UINT_MAX * UINT_MAX","vu32(0xFFFFFFFFu) * vu32(0xFFFFFFFFu)", u32(M32*M32))
emit(I,'u32',"0xDEADBEEF * 0xCAFEBABE","vu32(0xDEADBEEFu) * vu32(0xCAFEBABEu)", u32(0xDEADBEEF*0xCAFEBABE))
sec(I, "i32 signed division / remainder (truncate toward zero)")
for a, b in [(-7,2),(7,-2),(-7,-2),(7,2),(-2**31,2),(-2**31,2**31-1),(2**31-1,-1),(-2**31,1),(1,-2**31),(0,-5),(-2**31+1,-1)]:
    emit(I,'i32',f"{a} / {b}",f"v32({l32(a)}) / v32({l32(b)})", s32(tdiv(a,b)))
for a, b in [(-7,2),(7,-2),(-7,-2),(-2**31,-1),(-2**31,2**31-1),(2**31-1,-2**31),(5,-5),(-2**31,2),(-1,2**31-1)]:
    emit(I,'i32',f"{a} % {b}",f"srem32(v32({l32(a)}), v32({l32(b)}))", s32(trem(a,b)))
sec(I, "i32 unsigned division / remainder")
for a, b in [(M32,3),(2**31,M32),(M32,M32),(1,M32),(M32,2**31)]:
    emit(I,'u32',f"{a:#x} /u {b:#x}",f"vu32({lu32(a)}) / vu32({lu32(b)})", a//b)
for a, b in [(M32,10),(2**31,2**31-1),(M32,M32),(M32-1,M32),(12345,1)]:
    emit(I,'u32',f"{a:#x} %u {b:#x}",f"vu32({lu32(a)}) % vu32({lu32(b)})", a%b)
sec(I, "i32 shifts (count is taken modulo 32 by wasm)")
for x, n in [(1,31),(1,32),(1,33),(1,63),(1,M32),(M32,0),(0x12345678,4),(0x12345678,36)]:
    emit(I,'u32',f"{x:#x} << {n}",f"vu32({lu32(x)}) << vu32({lu32(n)})", u32(x << (n % 32)))
for x, n in [(2**31,31),(2**31,32),(2**31,33),(M32,1),(M32,31),(0x12345678,64)]:
    emit(I,'u32',f"{x:#x} >>u {n}",f"vu32({lu32(x)}) >> vu32({lu32(n)})", x >> (n % 32))
for x, n in [(-2**31,31),(-2**31,32),(-2**31,33),(-1,31),(2**31-1,31),(2**31-1,32),(-8,1),(-9,1)]:
    emit(I,'i32',f"{x} >>s {n}",f"v32({l32(x)}) >> vu32({lu32(n)})", x >> (n % 32))
sec(I, "i32 rotate / clz / ctz / popcnt")
for x, n in [(0x80000001,1),(0x80000001,32),(0x80000001,33),(0x12345678,8),(0x12345678,0),(0x12345678,31)]:
    emit(I,'u32',f"rotl({x:#x},{n})",f"__builtin_rotateleft32(vu32({lu32(x)}), vu32({lu32(n)}))", rotl(x,n,32))
    emit(I,'u32',f"rotr({x:#x},{n})",f"__builtin_rotateright32(vu32({lu32(x)}), vu32({lu32(n)}))", rotr(x,n,32))
for x in [0,1,0x80000000,0xFFFF,0x00010000,M32]:
    emit(I,'i32',f"clz({x:#x})",f"c_clz32(vu32({lu32(x)}))", clz(x,32))
    emit(I,'i32',f"ctz({x:#x})",f"c_ctz32(vu32({lu32(x)}))", ctz(x,32))
    emit(I,'i32',f"popcnt({x:#x})",f"__builtin_popcount(vu32({lu32(x)}))", pop(x))
emit(I,'i32',"popcnt(0xAAAAAAAA)","__builtin_popcount(vu32(0xAAAAAAAAu))", 16)
sec(I, "i32 sign / zero extension")
emit(I,'i32',"(int8)0x80","(int32_t)(int8_t)vu32(0x80u)", -128)
emit(I,'i32',"(int8)0x7F","(int32_t)(int8_t)vu32(0x7Fu)", 127)
emit(I,'i32',"(int8)0x1FF","(int32_t)(int8_t)vu32(0x1FFu)", -1)
emit(I,'i32',"(int16)0x8000","(int32_t)(int16_t)vu32(0x8000u)", -32768)
emit(I,'i32',"(int16)0xFFFF","(int32_t)(int16_t)vu32(0xFFFFu)", -1)
emit(I,'u32',"(uint8)0x1FF","(uint32_t)(uint8_t)vu32(0x1FFu)", 255)
emit(I,'u32',"(uint16)0x1FFFF","(uint32_t)(uint16_t)vu32(0x1FFFFu)", 65535)
sec(I, "i32 comparisons")
for nm, ce, ex in [
    ("(int)0x80000000 < 0", "(int32_t)((int32_t)vu32(0x80000000u) < 0)", 1),
    ("(uint)0x80000000 > 1", "(int32_t)(vu32(0x80000000u) > vu32(1u))", 1),
    ("INT_MIN < INT_MAX (signed)", "(int32_t)(v32(-2147483647-1) < v32(2147483647))", 1),
    ("INT_MIN > INT_MAX (unsigned)", "(int32_t)(vu32(0x80000000u) > vu32(0x7FFFFFFFu))", 1),
    ("0xFFFFFFFF < 0 (unsigned)", "(int32_t)(vu32(0xFFFFFFFFu) < vu32(0u))", 0),
    ("-1 < 1 (signed)", "(int32_t)(v32(-1) < v32(1))", 1),
    ("0xFFFFFFFF < 1 (unsigned)", "(int32_t)(vu32(0xFFFFFFFFu) < vu32(1u))", 0),
    ("INT_MIN == -INT_MIN", "(int32_t)(vu32(0x80000000u) == (0u - vu32(0x80000000u)))", 1)]:
    emit(I,'i32',nm,ce,ex)
sec(I, "i32 bit tricks")
emit(I,'u32',"bswap32(0x12345678)","__builtin_bswap32(vu32(0x12345678u))", 0x78563412)
emit(I,'u32',"bitreverse(1)","c_rev32(vu32(1u))", 0x80000000)
emit(I,'u32',"bitreverse(0x0F0F0F0F)","c_rev32(vu32(0x0F0F0F0Fu))", 0xF0F0F0F0)
emit(I,'u32',"bitreverse(0x12345678)","c_rev32(vu32(0x12345678u))", int(f"{0x12345678:032b}"[::-1],2))
emit(I,'u32',"abs(INT_MIN) in two's complement","c_abs32(v32(-2147483647-1))", 0x80000000)
for x in [3, 123456789, 0xFFFFFFFF, 0x80000001]:
    emit(I,'u32',f"modular inverse of {x:#x} mod 2^32",f"modinv32(vu32({lu32(x)}))", pow(x,-1,2**32))
    emit(I,'u32',f"  check {x:#x} * inverse",f"vu32({lu32(x)}) * modinv32(vu32({lu32(x)}))", 1)
sec(I, "i32 integer square root (Newton)")
for x in [0,1,2,3,4,15,16,17,0x7FFFFFFF,0xFFFE0000,0xFFFE0001,M32]:
    emit(I,'u32',f"isqrt32({x:#x})",f"isqrt32(vu32({lu32(x)}))", math.isqrt(x))
sec(I, "i32 loops (wrapping sums, gcd, collatz)")
emit(I,'u32',"sum of first 70000 odd numbers mod 2^32","sum_odd32(vu32(70000u))", u32(70000*70000))
emit(I,'u32',"sum i*i, i<1000","sum_sq32(vu32(1000u))", sum(i*i for i in range(1000)))
emit(I,'u32',"sum i, i<200000 (wraps)","sum_lin32(vu32(200000u))", u32(sum(range(200000))))
emit(I,'u32',"popcount sum 0..1023","popsum32(vu32(1024u))", sum(pop(i) for i in range(1024)))
emit(I,'u32',"gcd(1071,462)","gcd32(vu32(1071u), vu32(462u))", math.gcd(1071,462))
emit(I,'u32',"gcd(0xFFFFFFFF,0x7FFFFFFF)","gcd32(vu32(0xFFFFFFFFu), vu32(0x7FFFFFFFu))", math.gcd(M32,0x7FFFFFFF))
emit(I,'u32',"gcd(2^31,2^30*3)","gcd32(vu32(0x80000000u), vu32(0xC0000000u))", math.gcd(2**31,0xC0000000))
def collatz(n):
    steps=0; peak=n
    while n!=1:
        n = 3*n+1 if n&1 else n>>1; steps+=1; peak=max(peak,n)
    return steps, peak
st, pk = collatz(837799)
emit(I,'u32',"collatz(837799) steps","collatz32(vu32(837799u), &pk32)", st)
emit(I,'u32',"collatz(837799) peak (needs >2^31)","pk32", pk)

# ------------------------------------------------------------------ i64
J = []
sec(J, "i64 wraparound")
emit(J,'i64',"INT64_MAX + 1","(int64_t)(vu64(0x7FFFFFFFFFFFFFFFull) + vu64(1ull))", s64(2**63))
emit(J,'i64',"INT64_MIN - 1","(int64_t)(vu64(0x8000000000000000ull) - vu64(1ull))", s64(-2**63-1))
emit(J,'i64',"0 - INT64_MIN","(int64_t)(vu64(0ull) - vu64(0x8000000000000000ull))", s64(2**63))
emit(J,'i64',"INT64_MIN * -1","(int64_t)(vu64(0x8000000000000000ull) * vu64(0xFFFFFFFFFFFFFFFFull))", s64(2**63))
emit(J,'u64',"2^32 * 2^32","vu64(0x100000000ull) * vu64(0x100000000ull)", 0)
emit(J,'u64',"0xFFFFFFFF * 0xFFFFFFFF","vu64(0xFFFFFFFFull) * vu64(0xFFFFFFFFull)", 0xFFFFFFFE00000001)
emit(J,'u64',"UINT64_MAX * UINT64_MAX","vu64(0xFFFFFFFFFFFFFFFFull) * vu64(0xFFFFFFFFFFFFFFFFull)", u64(M64*M64))
emit(J,'u64',"0xDEADBEEFCAFEBABE * 0x123456789ABCDEF0","vu64(0xDEADBEEFCAFEBABEull) * vu64(0x123456789ABCDEF0ull)", u64(0xDEADBEEFCAFEBABE*0x123456789ABCDEF0))
emit(J,'u64',"6364136223846793005 * 3","vu64(6364136223846793005ull) * vu64(3ull)", u64(6364136223846793005*3))
sec(J, "i64 high word of 64x64 multiply (mulhi)")
for a, b in [(M64,M64),(2**63,2),(M64,2),(0x123456789ABCDEF0,0xFEDCBA9876543210),(1,M64),(0xFFFFFFFF,0xFFFFFFFF),(2**32,2**32)]:
    emit(J,'u64',f"mulhi({a:#x},{b:#x})",f"mulhi64(vu64({lu64(a)}), vu64({lu64(b)}))", (a*b)>>64)
sec(J, "i64 signed division / remainder")
for a, b in [(-7,2),(7,-2),(-7,-2),(-2**63,2),(-2**63,2**63-1),(2**63-1,-1),(-2**63,1),(1,-2**63),(-2**63+1,-1),(-9223372036854775807,3)]:
    emit(J,'i64',f"{a} / {b}",f"v64({l64(a)}) / v64({l64(b)})", s64(tdiv(a,b)))
for a, b in [(-7,2),(7,-2),(-2**63,-1),(-2**63,2**63-1),(2**63-1,-2**63),(5,-5),(-2**63,3)]:
    emit(J,'i64',f"{a} % {b}",f"srem64(v64({l64(a)}), v64({l64(b)}))", s64(trem(a,b)))
sec(J, "i64 unsigned division / remainder")
for a, b in [(M64,3),(M64,10),(2**63,M64),(M64,M64),(M64,2**32),(M64,6700417)]:
    emit(J,'u64',f"{a:#x} /u {b:#x}",f"vu64({lu64(a)}) / vu64({lu64(b)})", a//b)
    emit(J,'u64',f"{a:#x} %u {b:#x}",f"vu64({lu64(a)}) % vu64({lu64(b)})", a%b)
sec(J, "i64 shifts (count modulo 64)")
for x, n in [(1,63),(1,64),(1,65),(1,127),(1,M64),(M64,0),(0x0123456789ABCDEF,4),(0x0123456789ABCDEF,68)]:
    emit(J,'u64',f"{x:#x} << {n}",f"vu64({lu64(x)}) << vu64({lu64(n)})", u64(x << (n % 64)))
for x, n in [(2**63,63),(2**63,64),(2**63,65),(M64,1),(M64,63)]:
    emit(J,'u64',f"{x:#x} >>u {n}",f"vu64({lu64(x)}) >> vu64({lu64(n)})", x >> (n % 64))
for x, n in [(-2**63,63),(-2**63,64),(-2**63,65),(-1,63),(2**63-1,63),(2**63-1,64),(-9,1)]:
    emit(J,'i64',f"{x} >>s {n}",f"v64({l64(x)}) >> vu64({lu64(n)})", x >> (n % 64))
sec(J, "i64 rotate / clz / ctz / popcnt")
for x, n in [(0x8000000000000001,1),(0x8000000000000001,64),(0x8000000000000001,65),(0x0123456789ABCDEF,12),(0x0123456789ABCDEF,0)]:
    emit(J,'u64',f"rotl({x:#x},{n})",f"__builtin_rotateleft64(vu64({lu64(x)}), vu64({lu64(n)}))", rotl(x,n,64))
    emit(J,'u64',f"rotr({x:#x},{n})",f"__builtin_rotateright64(vu64({lu64(x)}), vu64({lu64(n)}))", rotr(x,n,64))
for x in [0,1,2**63,0xFFFF,2**52,M64,0x5555555555555555]:
    emit(J,'i64',f"clz64({x:#x})",f"c_clz64(vu64({lu64(x)}))", clz(x,64))
    emit(J,'i64',f"ctz64({x:#x})",f"c_ctz64(vu64({lu64(x)}))", ctz(x,64))
    emit(J,'i64',f"popcnt64({x:#x})",f"__builtin_popcountll(vu64({lu64(x)}))", pop(x))
sec(J, "i64 extension / wrap / comparison")
emit(J,'i64',"(int64)(int32)0x80000000","(int64_t)(int32_t)vu32(0x80000000u)", -2147483648)
emit(J,'i64',"(int64)(uint32)0xFFFFFFFF","(int64_t)(uint32_t)vu32(0xFFFFFFFFu)", 4294967295)
emit(J,'i64',"(int64)(int8)0x80","(int64_t)(int8_t)vu32(0x80u)", -128)
emit(J,'i64',"(int64)(int16)0x8000","(int64_t)(int16_t)vu32(0x8000u)", -32768)
emit(J,'u32',"(uint32)0x1FFFFFFFF (wrap)","(uint32_t)vu64(0x1FFFFFFFFull)", 0xFFFFFFFF)
emit(J,'i32',"(int32)0x123456789ABCDEF0 (wrap)","(int32_t)vu64(0x123456789ABCDEF0ull)", s32(0x9ABCDEF0))
for nm, ce, ex in [
    ("INT64_MIN < 0 (signed)", "(int32_t)(v64(-9223372036854775807ll-1) < v64(0))", 1),
    ("0x8000000000000000 > 1 (unsigned)", "(int32_t)(vu64(0x8000000000000000ull) > vu64(1ull))", 1),
    ("UINT64_MAX == -1", "(int32_t)(vu64(0xFFFFFFFFFFFFFFFFull) == (uint64_t)v64(-1))", 1),
    ("INT64_MAX < INT64_MIN (unsigned view)", "(int32_t)((uint64_t)v64(9223372036854775807ll) < (uint64_t)v64(-9223372036854775807ll-1))", 1)]:
    emit(J,'i32',nm,ce,ex)
J_SPLIT = len(J)
sec(J, "i64 bit tricks")
emit(J,'u64',"bswap64(0x0123456789ABCDEF)","__builtin_bswap64(vu64(0x0123456789ABCDEFull))", 0xEFCDAB8967452301)
emit(J,'u64',"bitreverse64(1)","c_rev64(vu64(1ull))", 2**63)
emit(J,'u64',"bitreverse64(0x0123456789ABCDEF)","c_rev64(vu64(0x0123456789ABCDEFull))", int(f"{0x0123456789ABCDEF:064b}"[::-1],2))
for x in [3, 6364136223846793005, M64]:
    emit(J,'u64',f"modular inverse of {x:#x} mod 2^64",f"modinv64(vu64({lu64(x)}))", pow(x,-1,2**64))
sec(J, "i64 number theory")
def fibn(n):
    a,b=0,1
    for _ in range(n): a,b=b,a+b
    return a
emit(J,'u64',"fib(92)","fib64(vu32(92u))", fibn(92))
emit(J,'u64',"fib(93) (largest that fits u64)","fib64(vu32(93u))", fibn(93))
emit(J,'u64',"fib(94) wraps mod 2^64","fib64(vu32(94u))", u64(fibn(94)))
emit(J,'u64',"20!","fact64(vu32(20u))", math.factorial(20))
emit(J,'u64',"21! mod 2^64","fact64(vu32(21u))", u64(math.factorial(21)))
emit(J,'u64',"25! mod 2^64 (trailing zero bits saturate)","fact64(vu32(25u))", u64(math.factorial(25)))
emit(J,'u64',"66! mod 2^64 (= 0)","fact64(vu32(66u))", u64(math.factorial(66)))
for x in [M64, 2**62, 2**63, 2**63-1, 10**18, 4503599627370496]:
    emit(J,'u64',f"isqrt64({x})",f"isqrt64(vu64({lu64(x)}))", math.isqrt(x))
for a, b in [(M64,6),(M64,2**32-1),(2**63,3*2**40),(1071,462),(0,7),(6700417*641,6700417*65537)]:
    emit(J,'u64',f"gcd64({a},{b})",f"gcd64(vu64({lu64(a)}), vu64({lu64(b)}))", math.gcd(a,b))
x = 0
for _ in range(1000): x = u64(6364136223846793005*x + 1442695040888963407)
emit(J,'u64',"MMIX LCG, 1000 steps from 0","lcg_mmix(vu32(1000u))", x)
m = 18446744073709551557
for a, b in [(m-1,m-1),(m-1,m-2),(0xDEADBEEFCAFEBABE,0x123456789ABCDEF0),(2**63,2**63)]:
    emit(J,'u64',f"mulmod({a:#x},{b:#x}) mod 2^64-59",f"mulmod64(vu64({lu64(a)}), vu64({lu64(b)}), vu64({lu64(m)}))", (a*b)%m)
for a, b, mm in [(123456789012,987654321098,1000000007),(2**40,2**41,2**61-1)]:
    emit(J,'u64',f"mulmod({a},{b}) mod {mm}",f"mulmod64(vu64({lu64(a)}), vu64({lu64(b)}), vu64({lu64(mm)}))", (a*b)%mm)
emit(J,'u64',"modpow(2, 2^64-60, 2^64-59) (Fermat)","modpow64(vu64(2ull), vu64(18446744073709551556ull), vu64(18446744073709551557ull))", pow(2,m-1,m))
for n0 in [63728127, 989345275647, 4611686018427387903]:
    st, pk = collatz(n0)
    if pk < 2**64:
        emit(J,'u64',f"collatz({n0}) steps",f"collatz64(vu64({lu64(n0)}), &pk64)", st)
        emit(J,'u64',f"collatz({n0}) peak",f"pk64", pk)

# ------------------------------------------------------------------ SHA / ChaCha / Miller-Rabin
def icbrt(n):
    x = 1 << ((n.bit_length() + 2) // 3)
    while True:
        y = (2*x + n//(x*x)) // 3
        if y >= x: return x
        x = y
primes = []
c = 2
while len(primes) < 80:
    if all(c % p for p in primes if p*p <= c): primes.append(c)
    c += 1
K256 = [icbrt(p << 96) & M32 for p in primes[:64]]
K512 = [icbrt(p << 192) & M64 for p in primes[:80]]
H256 = [math.isqrt(p << 64) & M32 for p in primes[:8]]
H512 = [math.isqrt(p << 128) & M64 for p in primes[:8]]
assert K256[0] == 0x428a2f98 and K512[0] == 0x428a2f98d728ae22 and H256[0] == 0x6a09e667 and H512[0] == 0x6a09e667f3bcc908

tabs = []
tabs.append("static const uint32_t K256[64] = {" + ",".join(f"0x{v:08x}u" for v in K256) + "};")
tabs.append("static const uint32_t H256[8] = {" + ",".join(f"0x{v:08x}u" for v in H256) + "};")
tabs.append("static const uint64_t K512[80] = {" + ",".join(f"0x{v:016x}ull" for v in K512) + "};")
tabs.append("static const uint64_t H512[8] = {" + ",".join(f"0x{v:016x}ull" for v in H512) + "};")
open("src/tables.inc", "w").write("\n".join(tabs) + "\n")

msgs = [b"", b"abc", b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
        b"The quick brown fox jumps over the lazy dog", bytes(range(256)) * 2 + b"x"]
H = []
H.append('    ew_println("-- SHA-256 / SHA-512 known-answer tests (32-bit and 64-bit rotates, adds, shifts)");')
for i, m in enumerate(msgs):
    cs = ",".join(str(b) for b in m) if m else "0"
    H.append(f'    {{ static const uint8_t m{i}[] = {{{cs}}}; sha256_hex(m{i}, {len(m)}u, hx);')
    H.append(f'      chk_str("SHA-256 of {len(m)}-byte message #{i}", hx, "{hashlib.sha256(m).hexdigest()}");')
    H.append(f'      sha512_hex(m{i}, {len(m)}u, hx);')
    H.append(f'      chk_str("SHA-512 of {len(m)}-byte message #{i}", hx, "{hashlib.sha512(m).hexdigest()}"); }}')
open("src/sha_gen.inc", "w").write("\n".join(H) + "\n")

# ChaCha20 block (RFC 8439 sec 2.3.2): key 00..1f, counter 1, nonce 00 00 00 09 00 00 00 4a 00 00 00 00
def chacha_block(key, counter, nonce):
    def qr(s, a, b, c, d):
        s[a]=u32(s[a]+s[b]); s[d]=rotl(s[d]^s[a],16,32)
        s[c]=u32(s[c]+s[d]); s[b]=rotl(s[b]^s[c],12,32)
        s[a]=u32(s[a]+s[b]); s[d]=rotl(s[d]^s[a],8,32)
        s[c]=u32(s[c]+s[d]); s[b]=rotl(s[b]^s[c],7,32)
    st = [0x61707865,0x3320646e,0x79622d32,0x6b206574] + list(struct.unpack('<8I', key)) + [counter] + list(struct.unpack('<3I', nonce))
    w = st[:]
    for _ in range(10):
        qr(w,0,4,8,12); qr(w,1,5,9,13); qr(w,2,6,10,14); qr(w,3,7,11,15)
        qr(w,0,5,10,15); qr(w,1,6,11,12); qr(w,2,7,8,13); qr(w,3,4,9,14)
    return [u32(w[i]+st[i]) for i in range(16)]
key = bytes(range(32)); nonce = bytes([0,0,0,9,0,0,0,0x4a,0,0,0,0])
blk = chacha_block(key, 1, nonce)
assert blk[0] == 0xe4e7f110 and blk[1] == 0x15593bd1   # RFC 8439 test vector
cc = ["    ew_println(\"-- ChaCha20 block, RFC 8439 section 2.3.2 test vector\");",
      "    { uint32_t blk[16]; int i; chacha20_block(blk);",
      "      for (i = 0; i < 16; i++) { char nm[24]; nm[0]='w'; nm[1]='o'; nm[2]='r'; nm[3]='d'; nm[4]=' '; nm[5]=(char)('0'+i/10); nm[6]=(char)('0'+i%10); nm[7]=0;",
      "        chk_u32(nm, blk[i], 0); }",
      "    }"]
# emit as explicit table compare instead of 16 zero placeholders
cc = ['    ew_println("-- ChaCha20 block, RFC 8439 section 2.3.2 test vector");',
      "    { static const uint32_t exp[16] = {" + ",".join(f"0x{v:08x}u" for v in blk) + "};",
      "      uint32_t blk[16]; int i; chacha20_block(blk);",
      "      for (i = 0; i < 16; i++) { char nm[16]; nm[0]='w'; nm[1]='o'; nm[2]='r'; nm[3]='d'; nm[4]=' '; nm[5]=(char)('0'+i/10); nm[6]=(char)('0'+i%10); nm[7]=0; chk_u32(nm, blk[i], exp[i]); } }"]
open("src/chacha_gen.inc", "w").write("\n".join(cc) + "\n")

def is_prime_mr(n):
    if n < 2: return False
    for p in [2,3,5,7,11,13,17,19,23,29,31,37]:
        if n % p == 0: return n == p
    d, r = n-1, 0
    while d % 2 == 0: d //= 2; r += 1
    for a in [2,3,5,7,11,13,17,19,23,29,31,37]:
        x = pow(a, d, n)
        if x in (1, n-1): continue
        for _ in range(r-1):
            x = x*x % n
            if x == n-1: break
        else: return False
    return True
mr_nums = [2, 561, 3215031751, 1000000007, 4294967291, 4294967297, 2**61-1, 3825123056546413051,
           9223372036854775783, 18446744073709551557, M64, 18446744073709551533]
for n in mr_nums:
    pass
MR = ['    ew_println("-- Miller-Rabin primality, full 64-bit range (deterministic, 12 bases)");']
for n in mr_nums:
    MR.append(f'    chk_i32("is_prime({n})", is_prime64(vu64({lu64(n)})), {1 if is_prime_mr(n) else 0});')
open("src/mr_gen.inc", "w").write("\n".join(MR) + "\n")

open("src/edge_i32_gen.inc", "w").write("\n".join(out_i) + "\n")
open("src/edge_i64a_gen.inc", "w").write("\n".join(J[:J_SPLIT]) + "\n")
open("src/edge_i64b_gen.inc", "w").write("\n".join(J[J_SPLIT:]) + "\n")
print("int checks:", len(out_i), " i64 checks:", len(J), " MR:", len(mr_nums), " SHA tests:", len(msgs)*2)
