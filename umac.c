#include <stddef.h>
#include "umac.h"

static uint32_t
pack_bigendian(const uint8_t *v)
{
#ifdef BIG_ENDIAN
  return *((uint32_t*)v);
#else
  return (uint32_t)v[0] << 24
      | (uint32_t)v[1] << 16
      | (uint32_t)v[2] << 8
      | (uint32_t)v[3];
#endif
}

static uint64_t
nh_iteration(const uint32_t* key, const uint32_t* msg)
{
  uint64_t y = 0;
  int j;
  for(j = 0; j < 4; ++j)
    y += (uint64_t)(msg[j] + key[j])
      * (uint64_t)(msg[4 + j] + key[4 + j]);

  return y;
}

static uint64_t
l1_hash_full_iteration(const uint32_t* key,
		       const uint32_t* msg)
{
  int i;
  uint64_t y = 0;
  for(i = 0; i < 256; i += 8)
    y += nh_iteration(key + i, msg + i);

  return y + 8192u;
}

static uint64_t
l1_hash_partial_iteration(const uint32_t* key,
			  const uint32_t* msg, size_t byte_len)
{
  int i;
  int word_len = (byte_len / 4) + (byte_len % 4 ? 1 : 0);
  int remainder = word_len % 8;
  int full_chunks = word_len - remainder;

  uint64_t y = 0;

  for(i = 0; i < full_chunks; i += 8)
    y += nh_iteration(key + i, msg + i);

  if(remainder)
    {
      uint32_t padded_msg[8] = {0, 0, 0, 0,
				0, 0, 0, 0};
      int j;
      for(j = 0; i < remainder; ++j)
	padded_msg[j] = msg[i+j];

      y += nh_iteration(key + i, padded_msg);
    }

  return y + (byte_len * 8u);
}

typedef struct
{
  uint64_t v[2]; /* Big endian; 0: most significant; 1: least significant */
} uint128;

static void
mul64(uint64_t a, uint64_t b, uint128 *out)
{
  static const uint64_t b32_mask = ((uint64_t)1u << 32) - 1;
  uint64_t x0, x1, y0, y1;
  uint64_t tmp0, tmp1;

  x0 = a & b32_mask;
  x1 = a >> 32;
  y0 = b & b32_mask;
  y1 = b >> 32;

  /* Final multiplication: out.v[0] * 2^64 + (tmp0 + tmp1) * 2^32 + out.v[1]. */
  out->v[1] = x0 * y0;
  out->v[0] = x1 * y1;
  tmp0 = x1 * y0;
  tmp1 = x0 * y1;

  /* Sum tmp0 and tmp1 into a single 64 bits number (may carry). */
  tmp0 += tmp1;

  /* Add up least significant 32 bit part (may carry). */
  uint64_t least = tmp0 << 32;
  out->v[1] += least;

  /* Add up most significant 32 bit part and possible carries.
     This will not overflow, because the result final result can't be
     bigger than 128 bits. */
  out->v[0] += (tmp0 >> 32)
    + (tmp0 < tmp1 ? ((uint64_t)1u << 32) : 0) /* tmp0 + tmp1 carry */
    + (out->v[1] < least); /* out.v[1] + least carry */
}

static const uint64_t offset_p64 = 59;
static const uint64_t p64 = (uint64_t)0u - 59;

static uint64_t
sum_mod_p64(uint64_t x, uint64_t y)
{
  uint64_t lower = x + y;
  if(lower < x)
    return (lower - p64) % p64;
  else
    return lower % p64;
}

static uint64_t
mul_mod_p64(uint64_t x, uint64_t y)
{
  uint128 mul;
  mul64(x, y, &mul);

  uint64_t ret;

  /* Mod p64 of the most significative nibble, (may recurse).
     Can't prove right now, but if offset_p64 is small enough,
     recursion will stop very fast.*/
  if(mul.v[0] > 312656679215416129)
    /* If greater than the biggest number that multiplied by offset_p64
     * still fits in 64 bits, recurse. */
    ret = mul_mod_p64(mul.v[0], offset_p64);
  else
    ret = mul.v[0] * offset_p64;

  ret = sum_mod_p64(ret, mul.v[1]);

  return ret;
}

static uint64_t
poly64_iteration(uint64_t key, uint64_t m, uint64_t y)
{
  const uint64_t marker = p64 - 1;
  const uint64_t maxwordrange = (uint64_t)0u - ((uint64_t)1u << 32);

  y = mul_mod_p64(key, y);
  if(m >= maxwordrange)
    {
      y = sum_mod_p64(y, marker);
      y = sum_mod_p64(mul_mod_p64(key, y), m - offset_p64);
    }
  else
    y = sum_mod_p64(y, m);

  return y;
}

typedef struct {
  uint128 most;
  uint128 least;
} uint256;

static void
mul128(const uint128 *a, const uint128 *b, uint256 *out)
{
  static const uint64_t b32_mask = ((uint64_t)1u << 32) - 1;

  uint128 tmp0, tmp1;

  /* Final multiplication: most * 2^128 + (tmp0 + tmp1) * 2^64 + least. */
  mul64(a->v[1], b->v[1], &out->least);
  mul64(a->v[0], b->v[0], &out->most);
  mul64(a->v[0], b->v[1], &tmp0);
  mul64(a->v[1], b->v[0], &tmp1);

  int carry;

  /* Sum tmp0 and tmp1 into a single 128 bits number. */
  tmp0.v[1] += tmp1.v[1];
  carry = tmp0.v[1] < tmp1.v[1];

  tmp0.v[0] += tmp1.v[0];
  if(tmp0.v[0] < tmp1.v[0] || (carry && tmp0.v[0] == UINT64_MAX))
    /* If overflow, add carry to most significant word. */
    ++out->most.v[0];

  tmp0.v[0] += carry;

  /* Add up least significant 64 bit part of tmp (may carry). */
  out->least.v[0] += tmp0.v[1];
  carry = out->least.v[0] < tmp0.v[1];

  /* Add up most significant 64 bit part of tmp and the carrie. */
  out->most.v[1] += tmp0.v[0];
  if(out->most.v[1] < tmp0.v[0] || (carry && out->most.v[1] == UINT64_MAX))
    /* If overflow, add carry to most significant word. */
    ++out->most.v[0];

  out->most.v[1] += carry;
}

static const uint64_t offset_p128 = 159;
static const uint128 p128 = {{0xFFFFFFFFFFFFFFFF,
			      0xFFFFFFFFFFFFFF61}};

/**
 * @param x must be different from out.
 * @param y may be the same as out.
 * @param out may be the same as y.
 */
static void
sum_mod_p128(const uint128 *x, const uint128 *y, uint128 *out)
{
  /* First, easily compute least significant bits of sum. */
  out->v[1] = y->v[1] + x->v[1];
  register int carry = out->v[1] < x->v[1];

  /* Now the complicated most significant bits: */

  /* Add carry with first term nibble. */
  out->v[0] = carry + y->v[0];
  carry = out->v[0] < carry; /* Get carry of this operation. */

  /* Add result with second term nibble. */
  out->v[0] += x->v[0];
  carry |= out->v[0] < x->v[0]; /* Get new carry or keep old carry. */

  /* If overflow (has carry), take modulus. */
  if(carry)
    {
      out->v[1] += offset_p128;
      if(out->v[1] < offset_p128 && !++out->v[0])
          out->v[1] += offset_p128;
    }
  /* Otherwise, take modulus if needed. */
  else if(out->v[0] == p128.v[0] && out->v[1] >= p128.v[1])
    {
      out->v[0] = 0;
      out->v[1] -= p128.v[1];
    }
}

/**
 * @param x may be the same as out.
 * @param y may be the same as out.
 * @param out may be the same as x and/or y.
 */
static void
mul_mod_p128(const uint128 *x, const uint128 *y, uint128 *out)
{
  const uint128 offset = {{0, offset_p128}};
  uint256 mul;
  mul128(x, y, &mul);

  uint256 inter; /* Only inter.least is actually used. */

  /* Mod p128 of the most significative nibble, (may recurse).
     Can't prove right now, but if offset_p64 is small enough,
     recursion will stop very fast.*/
  if(mul.most.v[0] < 0x19c2d14ee4a1019u
     || (mul.most.v[0] == 0x19c2d14ee4a1019u
	 && mul.most.v[1] <= 0xc2d14ee4a1019c2du))
    /* If multiplication will fit, do simple multiplication. */
    /* TODO: could implement mul(u128, u64) -> u128, since we
       know result will be at most 128 bits. */
    mul128(&mul.most, &offset, &inter);
  else {
    /* Otherwise, recurse. */
    mul_mod_p128(&mul.most, &offset, &inter.least);
  }

  sum_mod_p128(&inter.least, &mul.least, out);
}

static void
poly128_iteration(const uint128 *key, const uint128 *m, uint128 *y)
{
  const uint64 maxwordrange_msw = 0xffffffff00000000u;

  mul_mod_p128(key, y, y);
  if(m.v[0] >= maxwordrange_msw)
    {
      sum_mod_p128(marker, y, y);
      mul_mod_p128(key, y, y);
      uint128 tmp;
      sum_mod_p128(m, minus_offset, tmp);
      sum_mod_p128(tmp, y, y);
    }
  else
    {
      sum_mod_p128(m, y, y);
    }
}

static void
uhash_iter(const uint8_t* l1key, const uint8_t* l2key,
	   const uint8_t* l3key1, const uint8_t* l3key2,
	   const uint8_t *msg, size_t len, uint8_t* out)
{
  
}

#include <stdio.h>
#include <stdio_ext.h>

int main()
{
  while(1)
    {
      uint128 e, f;
      if(4 == scanf("%lx %lx %lx %lx", &e.v[0], &e.v[1], &f.v[0], &f.v[1])) {
	uint128 res;
	mul_mod_p128(&e, &f, &res);
	printf("0x%016lx%016lx\n", res.v[0], res.v[1]);
	fflush(stdout);
	//mul64(a, b, &c);
	//printf("0x%lx%lxL\n", c.v[0], c.v[1]);
      } else {
	__fpurge(stdin);
      }
    }
}