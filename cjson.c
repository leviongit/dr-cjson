/* -*- c-mode -*- */

#include "dragonruby.h"
#include "mruby.h"
#include "mruby/array.h"
#include "mruby/hash.h"
#include "mruby/istruct.h"
#include "mruby/string.h"
#include "mruby/value.h"
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>
#include <assert.h>

#define usym(name) usym__##name##__
#define usymv(name) usym(val__##name)
#define declare_usym(name)                                                     \
  mrb_sym usym(name);                                                          \
  mrb_value usymv(name)
#define init_usym2(mrb, declname, symname)                                     \
  usym(declname) = mrb_intern_lit(mrb, symname);                               \
  usymv(declname) = mrb_symbol_value(usym(declname))
#define init_usym(mrb, name) init_usym2(mrb, name, #name)

#define SFUNC(name, _) declare_usym(name);
#include "symbols.inc"
#undef SFUNC

struct json_opts_t {
  uint8_t indent_width;
  uint8_t indent_depth;
  struct {
    uint8_t symbolize_keys : 1;
    uint8_t sym_ext : 1;
    uint8_t obj_ext : 1;
    uint8_t spc_nul : 1;
    uint8_t minify : 1;
    uint8_t int_lit_int : 1;
    uint8_t slurp : 1;
    uint8_t string_symbols : 1;
  };
};

#define alloc_init_json_opts_val(MRB, VAR, INITER)                             \
  do {                                                                         \
    struct RIStruct *VAR##_rb = (struct RIStruct *)mrb_obj_alloc(              \
        MRB, MRB_TT_ISTRUCT, json_opts_class);                                 \
    *(struct json_opts_t *)VAR##_rb->inline_data = INITER;                     \
    VAR = mrb_obj_value(VAR##_rb);                                             \
  } while (0)

#define unwrap_istruct_json_opts(var) (struct json_opts_t *)mrb_istruct_ptr(var)

static_assert(sizeof(struct json_opts_t) <= ISTRUCT_DATA_SIZE, "");

struct json_opts_t default_opts = {.indent_width = 4,
                                   .indent_depth = 0,
                                   .symbolize_keys = 0,
                                   .sym_ext = 0,
                                   .obj_ext = 0,
                                   .spc_nul = 0,
                                   .minify = 0,
                                   .int_lit_int = 0,
                                   .slurp = 0,
                                   .string_symbols = 0};

mrb_value default_opts_val;

struct json_opts_t minify_opts = {.indent_width = 0,
                                  .indent_depth = 0,
                                  .symbolize_keys = 0,
                                  .sym_ext = 0,
                                  .obj_ext = 0,
                                  .spc_nul = 0,
                                  .minify = 1,
                                  .int_lit_int = 0,
                                  .slurp = 0,
                                  .string_symbols = 0};

mrb_value minify_opts_val;

static struct RClass *parse_error_class;
static struct RClass *serialization_error_class;
static struct RClass *assert_failed_class;
static struct RClass *json_opts_class;

#define _DCJ_STRINGIFY(...) #__VA_ARGS__
#define DCJ_STRINGIFY(...) _DCJ_STRINGIFY(__VA_ARGS__)

struct dcj_parsing_ctx {
  const char *str;
  const char *stri;
  const char *send;
  const char *obeg;
  mrb_value co;
  const struct json_opts_t *opts;
  struct {
    uint8_t parsing_key : 1;
    uint8_t tosym : 1;
  };
};

#define dcj_bug(mrb, assert, msg, ...)                                         \
  do {                                                                         \
    if (!(assert)) {                                                           \
      mrb_raisef(mrb, assert_failed_class,                                     \
                 msg " in %s"                                                  \
                     " at line " DCJ_STRINGIFY(                                \
                         __LINE__) " with expr " DCJ_STRINGIFY(assert),        \
                 __VA_ARGS__ __VA_OPT__(, ) __func__);                         \
    }                                                                          \
  } while (0)

#if defined(DCJ_DEBUG) || 1
#define dcj_assert(mrb, assert, msg, ...)                                      \
  dcj_bug(mrb, assert, "assertion failed " msg, __VA_ARGS__)
#else
#define dcj_assert(...) (void)0
#endif

mrb_value dcj_json_opts_newc(mrb_state *mrb, struct json_opts_t opts) {
  mrb_value instance;
  alloc_init_json_opts_val(mrb, instance, opts);
  return instance;
}

mrb_value dcj_json_opts_new_m(mrb_state *mrb, mrb_value) {
  const mrb_sym s_table[] = {
      usym(indent_width), usym(symbolize_keys), usym(symbol_ext),
      usym(object_ext),   usym(space_in_empty), usym(minify),
      usym(integer_lits), usym(slurp),          usym(string_symbols)};
  mrb_value values[9];
  mrb_kwargs kws = {.values = values,
                    .num = 9,
                    .required = 0,
                    .rest = NULL,
                    .table = s_table};

  mrb_get_args(mrb, ":", &kws);

  mrb_value optsv;
  alloc_init_json_opts_val(mrb, optsv, default_opts);

  struct json_opts_t *opts = unwrap_istruct_json_opts(optsv);

  if (!mrb_undef_p(values[0])) {
    mrb_int iwidth = mrb_int(mrb, values[0]);
    if ((size_t)iwidth > 0xff) {
      dcj_bug(mrb, 0, "tried setting indent width to >255 or  <0");
    }
    opts->indent_width = (uint8_t)iwidth;
  }
  if (!mrb_undef_p(values[1])) {
    opts->symbolize_keys = mrb_test(values[1]);
  }
  if (!mrb_undef_p(values[2])) {
    opts->sym_ext = mrb_test(values[2]);
  }
  if (!mrb_undef_p(values[3])) {
    opts->obj_ext = mrb_test(values[3]);
  }
  if (!mrb_undef_p(values[4])) {
    opts->spc_nul = mrb_test(values[4]);
  }
  if (!mrb_undef_p(values[5])) {
    opts->minify = mrb_test(values[5]);
  }
  if (!mrb_undef_p(values[6])) {
    opts->int_lit_int = mrb_test(values[6]);
  }
  if (!mrb_undef_p(values[7])) {
    opts->slurp = mrb_test(values[7]);
  }
  if (!mrb_undef_p(values[8])) {
    opts->string_symbols = mrb_test(values[8]);
  }

  return optsv;
}

mrb_value dcj_json_opts_inspect(mrb_state *mrb, mrb_value self) {
  struct json_opts_t opts = *unwrap_istruct_json_opts(self);
  mrb_value rbool[2] = {mrb_false_value(), mrb_true_value()};
  return mrb_format(
      mrb,
      "%T(indent_width: %i, symbolize_keys: %v, symbol_ext: %v, object_ext: "
      "%v, space_in_empty: %v, minify: %v, integer_lits: %v, slurp: %v, "
      "string_symbols: %v)",
      self, (mrb_int)opts.indent_width, rbool[opts.symbolize_keys],
      rbool[opts.sym_ext], rbool[opts.obj_ext], rbool[opts.spc_nul],
      rbool[opts.minify], rbool[opts.int_lit_int], rbool[opts.slurp],
      rbool[opts.string_symbols]);
}

mrb_value dcj_json_opts_default(mrb_state *, mrb_value) {
  return default_opts_val;
}

mrb_value dcj_json_opts_minify(mrb_state *, mrb_value) {
  return minify_opts_val;
}

static _Bool dcj_is_whitespace(const char c) {
  return c == 0x09 || c == 0x0a || c == 0x0d || c == 0x20;
}

static _Bool dcj_is_digit(const char c) { return c >= '0' && c <= '9'; }

static _Bool dcj_is_simplestring_c(const char c) {
  return (uint8_t)c >= 0x20 && c != '"' && c != '\\';
}

[[noreturn]] mrb_value dcj_notimpl(mrb_state *mrb, mrb_value self) {
  mrb_sym mid = mrb->c->ci->mid;

  if (mrb_class_p(self) || mrb_module_p(self)) {
    mrb_raisef(mrb, E_NOTIMP_ERROR, "%C.%n is not yet implemented",
               mrb_class_ptr(self), mid);
  } else {
    mrb_raisef(mrb, E_NOTIMP_ERROR, "%Y#%n is not yet implemented", self, mid);
  }
}

mrb_value dcj_parse_value(mrb_state *mrb, struct dcj_parsing_ctx *ctx);

#define dcj_parser_peek(ctx) (((ctx)->stri < (ctx)->send) ? *(ctx)->stri : 0)
#if 1
#define dcj_parser_adv(ctx) ((ctx)->stri < (ctx)->send && ++(ctx)->stri)
#else
#define dcj_parser_adv(ctx) ++(ctx)->stri
#endif

#define dcj_buffer_remaining(ctx) ((ctx)->send - (ctx)->stri)
#define dcj_past_end(ctx) ((ctx)->stri >= (ctx)->send)

// crimes!
#define dcj_parser_match(ctx, chr)                                             \
  (dcj_parser_peek(ctx) == (chr) ? dcj_parser_adv(ctx) : 0)

#define dcj_parser_match_noadv(ctx, chr) (dcj_parser_peek(ctx) == (chr) ? 1 : 0)

#define dcj_parser_expect(mrb, ctx, chr)                                       \
  do {                                                                         \
    typeof(ctx) _ctx_ = (ctx);                                                 \
    char _chr_ = (chr);                                                        \
    if (!dcj_parser_match(_ctx_, _chr_)) {                                     \
      dcj_parser_raise_unexpected(mrb, _ctx_, DCJ_UT_EXP_CHR, _chr_);          \
    }                                                                          \
  } while (0)

// don't mutate ctx pls
#define dcj_parser_match_fn(ctx, pred)                                         \
  (pred(dcj_parser_peek(ctx)) ? dcj_parser_adv(ctx) : 0)

#define dcj_parser_match_noadv_fn(ctx, pred)                                   \
  (pred(dcj_parser_peek(ctx)) ? 1 : 0)

void dcj_skip_whitespace(struct dcj_parsing_ctx *ctx) {
  while (dcj_is_whitespace(dcj_parser_peek(ctx)))
    dcj_parser_adv(ctx);
}

_Bool dcj_parser_match_string(struct dcj_parsing_ctx *ctx, const char *str,
                              size_t len) {
  const char *const eptr = ctx->stri + len;
  const char *const send = str + len;

  while (str < send && dcj_parser_match(ctx, *str++)) {
  }

  return ctx->stri == eptr;
}

#define dcj_parser_match_lit(ctx, str)                                         \
  (dcj_parser_match_string(ctx, str "", sizeof(str "") - 1))

enum unexpected_reason : uint8_t {
  DCJ_UT_EXP_VAL,
  DCJ_UT_EXP_STR,
  DCJ_UT_UEXP_CC,
  DCJ_UT_EXP_NUM,
  DCJ_UT_EXP_ESC,
  DCJ_UT_EXP_EOF,
  DCJ_UT_UEXP_EOF,
  DCJ_UT_EXP_CHR,
  DCJ_UT_EXP_HEX,
  DCJ_UT_EXP_LSP,
  DCJ_UT_UEXP_HS,
  DCJ_UT_EXP_CSP,
};

/* this will require betterment */

[[noreturn]] void dcj_parser_raise_unexpected(mrb_state *mrb,
                                              struct dcj_parsing_ctx *ctx,
                                              enum unexpected_reason reason,
                                              char chr) {
#define raisefmt_char_int "'%c' [%d]"
#define char_int                                                               \
  (dcj_parser_peek(ctx)), ((mrb_int)(dcj_parser_peek(ctx) & 0xff))
  switch (reason) {
  case DCJ_UT_EXP_VAL:
    // buggy, fix it later
    mrb_raisef(mrb, parse_error_class,
               "expected a json value begin char (/[tfn\\{\\[0-9\"]/) "
               "got " raisefmt_char_int,
               char_int);
  case DCJ_UT_EXP_STR:
    mrb_raisef(mrb, parse_error_class,
               "expected a (string) '\"' got " raisefmt_char_int, char_int);
  case DCJ_UT_EXP_NUM:
    mrb_raisef(mrb, parse_error_class,
               "expected a number got " raisefmt_char_int, char_int);
  case DCJ_UT_EXP_ESC:
    mrb_raisef(mrb, parse_error_class,
               "expected a string escape char, got " raisefmt_char_int,
               char_int);
  case DCJ_UT_EXP_EOF:
    mrb_raisef(mrb, parse_error_class, "expected eof got " raisefmt_char_int,
               char_int);
  case DCJ_UT_UEXP_EOF:
    mrb_raise(mrb, parse_error_class, "unexpected eof");
  case DCJ_UT_EXP_CHR:
    mrb_raisef(mrb, parse_error_class,
               "[%i] expected '%c' got " raisefmt_char_int,
               ctx->stri - ctx->str, chr, char_int);
  case DCJ_UT_UEXP_CC:
    mrb_raisef(mrb, parse_error_class,
               "unexpected control character " raisefmt_char_int, char_int);
  case DCJ_UT_EXP_HEX:
    mrb_raisef(mrb, parse_error_class,
               "expected hex digit [0-9a-fA-F] got " raisefmt_char_int,
               char_int);
  case DCJ_UT_UEXP_HS:
    mrb_raisef(mrb, parse_error_class, "unexpected high surrogate");
  case DCJ_UT_EXP_CSP:
    mrb_raisef(mrb, parse_error_class, "expected complementary surrogate");
  case DCJ_UT_EXP_LSP:
    mrb_raisef(mrb, parse_error_class,
               "low surrogate not in low surrogate range");
  }
}

static uint8_t hexlut[256] = {[0 ... sizeof(hexlut) - 1] = (uint8_t)-1};

static uint8_t dcj_hextoi(const char c) { return hexlut[(uint8_t)c]; }

static size_t dcj_parse_hex_esc(mrb_state *mrb, struct dcj_parsing_ctx *ctx,
                                char buf[4]) {
  if (dcj_buffer_remaining(ctx) < 4) {
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);
  }

  uint32_t codepoint = 0;
  uint32_t tmpbufi;
  uint8_t tmpbuf[4];
  static_assert(sizeof(tmpbufi) == sizeof(tmpbuf), "");

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
  for (int8_t i = 3; i >= 0; --i) {
    tmpbuf[i] = dcj_hextoi(*ctx->stri++);
  }
#elif (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
  for (int8_t i = 0; i < 4; ++i) {
    tmpbuf[i] = dcj_hextoi(*ctx->stri++);
  }
#else
#error "unknown endianess"
#endif

  memcpy(&tmpbufi, tmpbuf, sizeof(tmpbuf));
  if (tmpbufi & 0xf0f0f0f0) {
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_HEX, 0);
  }

  tmpbufi &= 0x0f0f0f0f;
  tmpbufi = (tmpbufi | tmpbufi >> 4) & 0x00ff00ff;
  codepoint = (tmpbufi | tmpbufi >> 8) & 0x0000ffff;

  if (codepoint >= 0xd800 && codepoint <= 0xdfff) {
    if (codepoint >= 0xdc00)
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_HS, 0);
    codepoint &= 0x03ff;

    if (!dcj_parser_match_lit(ctx, "\\u") || dcj_buffer_remaining(ctx) < 4) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_CSP, 0);
    }

#if (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__)
    for (int8_t i = 3; i >= 0; --i) {
      tmpbuf[i] = dcj_hextoi(*ctx->stri++);
    }
#elif (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
    for (int8_t i = 0; i < 4; ++i) {
      tmpbuf[i] = dcj_hextoi(*ctx->stri++);
    }
#else
#error "unknown endianess"
#endif

    memcpy(&tmpbufi, tmpbuf, sizeof(tmpbuf));
    if (tmpbufi & 0xf0f0f0f0) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_HEX, 0);
    }

    tmpbufi &= 0x0f0f0f0f;
    tmpbufi = (tmpbufi | tmpbufi >> 4) & 0x00ff00ff;
    tmpbufi = (tmpbufi | tmpbufi >> 8) & 0x0000ffff;

    if (tmpbufi < 0xdc00 || tmpbufi > 0xdfff) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_LSP, 0);
    }

    codepoint = (codepoint << 10) | tmpbufi;
    codepoint = codepoint + 0x10000;
  }

  if (codepoint < 0x0080) {
    buf[0] = codepoint;
    return 1;
  } else if (codepoint < 0x0800) {
    buf[0] = 0xc0 | (codepoint & 0x03c0) >> 6;
    buf[1] = 0x80 | (codepoint & 0x003f);
    return 2;
  } else if (codepoint < 0x10000) {
    buf[0] = 0xe0 | (codepoint & 0xf000) >> 12;
    buf[1] = 0x80 | (codepoint & 0x0fc0) >> 6;
    buf[2] = 0x80 | (codepoint & 0x003f);
    return 3;
  } else {
    buf[0] = 0xf0 | (codepoint & 0x1c0000) >> 18;
    buf[1] = 0x80 | (codepoint & 0x03f000) >> 12;
    buf[2] = 0x80 | (codepoint & 0x000fc0) >> 6;
    buf[3] = 0x80 | (codepoint & 0x00003f);
    return 4;
  }
}

mrb_value dcj_parse_string(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  dcj_skip_whitespace(ctx);
  dcj_parser_expect(mrb, ctx, '"');

  const char *pbeg = ctx->stri;
  mrb_value str;

  /* bad */
  ctx->tosym = (ctx->opts->string_symbols && dcj_parser_match(ctx, ':') && (pbeg = ctx->stri)) ||
               (dcj_parser_match_lit(ctx, "@@jm:") && (ctx->stri = pbeg)) ||
               ctx->tosym;

  if ((ctx->opts->symbolize_keys && ctx->parsing_key) || ctx->tosym) {
    while (dcj_parser_match_fn(ctx, dcj_is_simplestring_c)) {
    }

    size_t len = ctx->stri - pbeg;
    if (dcj_parser_peek(ctx) == '"') {
      dcj_parser_adv(ctx);
      dcj_skip_whitespace(ctx);

      ctx->parsing_key = 0;
      ctx->tosym = 0;
      return mrb_symbol_value(mrb_intern(mrb, pbeg, len));
    }
    str = mrb_str_new(mrb, pbeg, len);
    goto fallback;
  }

  str = mrb_str_new_capa(mrb, 16);
  while (!(dcj_parser_match(ctx, '"') || dcj_past_end(ctx))) {
    pbeg = ctx->stri;
    while (dcj_parser_match_fn(ctx, dcj_is_simplestring_c)) {
    }

    size_t len = ctx->stri - pbeg;
    mrb_str_cat(mrb, str, pbeg, len);
  fallback:
    if (dcj_parser_peek(ctx) == '\\') {
      dcj_parser_adv(ctx);
      const char c = dcj_parser_peek(ctx);
      dcj_parser_adv(ctx);
      switch (c) {
      case '"':
      case '\\':
      case '/':
        mrb_str_cat(mrb, str, &c, 1);
        break;
      case 'b':
        mrb_str_cat_lit(mrb, str, "\b");
        break;
      case 'f':
        mrb_str_cat_lit(mrb, str, "\f");
        break;
      case 'n':
        mrb_str_cat_lit(mrb, str, "\n");
        break;
      case 'r':
        mrb_str_cat_lit(mrb, str, "\r");
        break;
      case 't':
        mrb_str_cat_lit(mrb, str, "\t");
        break;
      case 'u':
        char buf[4] = {};
        size_t seqlen = dcj_parse_hex_esc(mrb, ctx, buf);
        mrb_str_cat(mrb, str, buf, seqlen);
        break;
      default:
        dcj_bug(mrb, 0, "cannot recognise %c %d escape seq", c, c);
      }
    }
  }

  if ((ctx->opts->symbolize_keys && ctx->parsing_key) || ctx->tosym) {
    ctx->parsing_key = 0;
    ctx->tosym = 0;
    str = mrb_symbol_value(mrb_intern_str(mrb, str));
  }
  return str;
}

mrb_value dcj_parse_key(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  ctx->parsing_key = 1;
  return dcj_parse_string(mrb, ctx);
}

// inexact - slow - etc.
mrb_value dcj_parse_number(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  int8_t sign = 1;
  if (dcj_parser_match(ctx, '-'))
    sign = -1;

  const char *int_end = ctx->stri;
  double value = 0.0;

  if (dcj_parser_match(ctx, '0')) {
    int_end = ctx->stri;
    if (dcj_parser_match(ctx, '.'))
      goto l_float;
    else if (dcj_parser_match(ctx, 'e') || dcj_parser_match(ctx, 'E'))
      goto l_exp;
    else if (ctx->opts->int_lit_int)
      return mrb_int_value(mrb, 0);
    else
      return mrb_float_value(mrb, 0.0);
  }

  while (dcj_parser_match_noadv_fn(ctx, dcj_is_digit)) {
    value = value * 10.0 + ((*ctx->stri) - '0');
    dcj_parser_adv(ctx);
  }

  if (ctx->stri == int_end)
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_NUM, 0);

  int_end = ctx->stri;

  double divisor = 0.1;
  if (dcj_parser_match(ctx, '.')) {
  l_float:
    if (!dcj_parser_match_noadv_fn(ctx, dcj_is_digit)) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_NUM, 0);
    }

    while (dcj_parser_match_noadv_fn(ctx, dcj_is_digit)) {
      value += ((*ctx->stri) - '0') * divisor;
      divisor /= 10;
      dcj_parser_adv(ctx);
    }
  }

  if (dcj_parser_match(ctx, 'e') || dcj_parser_match(ctx, 'E')) {
  l_exp:
    int8_t expsgn = 1;
    mrb_int exponent = 0;

    if (dcj_parser_match(ctx, '-'))
      expsgn = -1;
    if (!dcj_parser_match_noadv_fn(ctx, dcj_is_digit)) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_NUM, 0);
    }

    while (dcj_parser_match_noadv_fn(ctx, dcj_is_digit)) {
      exponent = exponent * 10 + ((*ctx->stri) - '0');
      dcj_parser_adv(ctx);
    }
    value *= exp10((double)exponent * expsgn);
  }

  if (ctx->opts->int_lit_int && ctx->stri == int_end) {
    return mrb_int_value(mrb, (mrb_int)value * sign);
  } else {
    return mrb_float_value(mrb, value * sign);
  }
}

mrb_value dcj_parse_object(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  dcj_parser_expect(mrb, ctx, '{');
  dcj_skip_whitespace(ctx);
  if (dcj_parser_match(ctx, '}')) {
    return mrb_hash_new(mrb);
  }

  /* all this just to find special cases :< */
  mrb_value hash = mrb_nil_value();
  mrb_value key = dcj_parse_key(mrb, ctx);
  dcj_skip_whitespace(ctx);
  dcj_parser_expect(mrb, ctx, ':');
  dcj_skip_whitespace(ctx);
  if (mrb_obj_equal(mrb, key, usymv(jsonmarker_symbol)) && ctx->opts->sym_ext) {
    ctx->tosym = 1;
    mrb_value v = dcj_parse_string(mrb, ctx);
    dcj_skip_whitespace(ctx);
    dcj_parser_expect(mrb, ctx, '}');
    return v;
  }

  mrb_value val = dcj_parse_value(mrb, ctx);
  dcj_skip_whitespace(ctx);

  hash = mrb_hash_new(mrb);
  mrb_int ai = mrb_gc_arena_save(mrb);
  mrb_hash_set(mrb, hash, key, val);

  while (dcj_parser_match(ctx, ',')) {
    dcj_skip_whitespace(ctx);
    key = dcj_parse_key(mrb, ctx);
    dcj_skip_whitespace(ctx);
    dcj_parser_expect(mrb, ctx, ':');

    val = dcj_parse_value(mrb, ctx);
    dcj_skip_whitespace(ctx);

    mrb_hash_set(mrb, hash, key, val);
    mrb_gc_arena_restore(mrb, ai);
  }

  /* dcj_skip_whitespace(ctx); */
  dcj_parser_expect(mrb, ctx, '}');
  return hash;
}

mrb_value dcj_parse_array(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  dcj_parser_expect(mrb, ctx, '[');
  dcj_skip_whitespace(ctx);
  if (dcj_parser_match(ctx, ']')) {
    return mrb_ary_new(mrb);
  }

  mrb_value ary = mrb_ary_new(mrb);
  mrb_int ai = mrb_gc_arena_save(mrb);

  do {
    mrb_value elem = dcj_parse_value(mrb, ctx);
    mrb_ary_push(mrb, ary, elem);
    mrb_gc_arena_restore(mrb, ai);
    dcj_skip_whitespace(ctx);
  } while (dcj_parser_match(ctx, ','));

  dcj_parser_expect(mrb, ctx, ']');
  return ary;
}

mrb_value dcj_parse_value(mrb_state *mrb, struct dcj_parsing_ctx *ctx) {
  dcj_skip_whitespace(ctx);
  if (dcj_past_end(ctx))
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);

  switch (dcj_parser_peek(ctx)) {
  case 0:
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);
  case '-':
  case '0' ... '9':
    return dcj_parse_number(mrb, ctx);
  case '"':
    return dcj_parse_string(mrb, ctx);
  case '[':
    return dcj_parse_array(mrb, ctx);
  case '{':
    return dcj_parse_object(mrb, ctx);
  case 't':
    if (dcj_buffer_remaining(ctx) < 4) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);
    }

    dcj_parser_match_lit(ctx, "true");

    return mrb_true_value();
  case 'f':
    if (dcj_buffer_remaining(ctx) < 5) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);
    }

    dcj_parser_match_lit(ctx, "false");

    return mrb_false_value();
  case 'n':
    if (dcj_buffer_remaining(ctx) < 4) {
      dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_UEXP_EOF, 0);
    }

    dcj_parser_match_lit(ctx, "null");

    return mrb_nil_value();
  default:
    dcj_parser_raise_unexpected(mrb, ctx, DCJ_UT_EXP_VAL, dcj_parser_peek(ctx));
    __builtin_unreachable();
  }
}

mrb_value dcj_parse_json_m(mrb_state *mrb, mrb_value) {
  mrb_value rstr;
  mrb_value opts_v = mrb_nil_value();
  const struct json_opts_t *opts;
  mrb_get_args(mrb, "S|o", &rstr, &opts_v);

  const char *str = RSTRING_PTR(rstr);
  mrb_int len = RSTRING_LEN(rstr);

  if (mrb_nil_p(opts_v))
    opts_v = default_opts_val;
  if (!(mrb_istruct_p(opts_v) &&
        mrb_obj_class(mrb, opts_v) == json_opts_class)) {
    mrb_raisef(mrb, E_ARGUMENT_ERROR, "expected a %C instance, got %T",
               json_opts_class, opts_v);
  }
  opts = mrb_istruct_ptr(opts_v);

  struct dcj_parsing_ctx ctx = {.str = str,
                                .stri = str,
                                .send = str + len,
                                .obeg = NULL,
                                .co = mrb_nil_value(),
                                .opts = opts,
                                .parsing_key = 0};

  dcj_skip_whitespace(&ctx);
  if (!ctx.opts->slurp) {
    mrb_value ret = dcj_parse_value(mrb, &ctx);
    dcj_skip_whitespace(&ctx);
    if (!dcj_past_end(&ctx)) {
      dcj_parser_raise_unexpected(mrb, &ctx, DCJ_UT_EXP_EOF, 0);
    }
    return ret;
  } else {
    mrb_value ary = mrb_ary_new(mrb);
    if (dcj_past_end(&ctx)) {
      return ary;
    }

    mrb_int ai = mrb_gc_arena_save(mrb);
    while (!dcj_past_end(&ctx)) {
      mrb_ary_push(mrb, ary, dcj_parse_value(mrb, &ctx));
      mrb_gc_arena_restore(mrb, ai);
    }

    return ary;
  }
}

mrb_value true_string;
mrb_value false_string;
mrb_value null_string;

#define FLO_TO_STR_PREC 16
static mrb_value flo_to_s(mrb_state *mrb, mrb_value flt) {
  mrb_float f = mrb_float(flt);
  mrb_value str;

  if (isinf(f)) {
    str = f < 0 ? mrb_str_new_lit(mrb, "-Infinity")
                : mrb_str_new_lit(mrb, "Infinity");
    goto exit;
  } else if (isnan(f)) {
    str = mrb_str_new_lit(mrb, "NaN");
    goto exit;
  } else {
    char fmt[] = "%." MRB_STRINGIZE(FLO_TO_STR_PREC) "g";
    mrb_int len;
    char *begp, *p, *endp;

    str = mrb_float_to_str(mrb, flt, fmt);

  insert_dot_zero:
    begp = RSTRING_PTR(str);
    len = RSTRING_LEN(str);
    for (p = begp, endp = p + len; p < endp; ++p) {
      if (*p == '.') {
        goto exit;
      } else if (*p == 'e') {
        ptrdiff_t e_pos = p - begp;
        mrb_str_cat(mrb, str, ".0", 2);
        p = RSTRING_PTR(str) + e_pos;
        memmove(p + 2, p, len - e_pos);
        memcpy(p, ".0", 2);
        goto exit;
      }
    }

    if (FLO_TO_STR_PREC + (begp[0] == '-') <= len) {
      --fmt[sizeof(fmt) - 3]; /* %.16g(%.8g) -> %.15g(%.7g) */
      str = mrb_float_to_str(mrb, flt, fmt);
      goto insert_dot_zero;
    } else {
      mrb_str_cat(mrb, str, ".0", 2);
    }

    goto exit;
  }

exit:
  RSTR_SET_ASCII_FLAG(mrb_str_ptr(str));
  return str;
}

#define write_args(mrb, capa)                                                  \
  const struct json_opts_t *opts;                                              \
  mrb_value bufstr;                                                            \
  dcj_take_write_args(mrb, &opts, &bufstr, capa)

void dcj_take_write_args(mrb_state *mrb, const struct json_opts_t **opts,
                         mrb_value *bufstr, size_t capa) {
  mrb_value str;
  mrb_value opts_v = mrb_nil_value();
  mrb_int argc = mrb_get_args(mrb, "|oS", &opts_v, &str);

  switch (argc) {
  case 0:
  case 1:
    if (capa) {
      str = mrb_str_new_capa(mrb, capa == -1ull ? 16 : capa);
    } else {
      str = mrb_nil_value();
    }
  case 2:
    if (mrb_nil_p(opts_v))
      opts_v = default_opts_val;
    if (!(mrb_istruct_p(opts_v) &&
          mrb_obj_class(mrb, opts_v) == json_opts_class)) {
      mrb_raisef(mrb, E_ARGUMENT_ERROR, "expected a %C instance, got %T",
                 json_opts_class, opts_v);
    }
    *opts = mrb_istruct_ptr(opts_v);
    *bufstr = str;
    break;
  default:
    mrb_raisef(mrb, E_RUNTIME_ERROR, "[[bug]] somehow got argc of %i", argc);
    __builtin_unreachable();
  }
}

mrb_value dcj_generic_write_json(mrb_state *mrb, const struct json_opts_t *opts,
                                 mrb_value self, mrb_value bufstr,
                                 _Bool as_key);

mrb_value dcj_integer_write_json(mrb_state *mrb, mrb_value self,
                                 mrb_value bufstr) {
  if (mrb_nil_p(bufstr)) {
    return mrb_fixnum_to_str(mrb, self, 10);
  }
  dcj_assert(mrb, mrb_string_p(bufstr), "");
  return mrb_str_cat_str(mrb, bufstr, mrb_fixnum_to_str(mrb, self, 10));
}

mrb_value dcj_float_write_json(mrb_state *mrb, mrb_value self,
                               mrb_value bufstr) {
  if (mrb_nil_p(bufstr)) {
    return flo_to_s(mrb, self);
  }
  dcj_assert(mrb, mrb_string_p(bufstr), "");
  return mrb_str_cat_str(mrb, bufstr, flo_to_s(mrb, self));
}

mrb_value dcj_write_json_ptrlen_str(mrb_state *mrb, const char *str,
                                    const size_t len, mrb_value bufstr,
                                    _Bool is_symbol) {
  if (mrb_nil_p(bufstr)) {
    bufstr = mrb_str_new_capa(mrb, len + 2);
  }
  if (len == 0) {
    if (!is_symbol) {
      return mrb_str_cat_lit(mrb, bufstr, "\"\"");
    } else {
      return mrb_str_cat_lit(mrb, bufstr, "\":\"");
    }
  }

  const char *strp = str;
  const char *const stre = strp + len;

  mrb_str_cat_lit(mrb, bufstr, "\"");

  if (!is_symbol && *strp == ':') {
    mrb_str_cat_lit(mrb, bufstr, "\\u003a");
    ++strp;
  } else if (is_symbol) {
    mrb_str_cat_lit(mrb, bufstr, ":");
  }

  const char *pbeg = strp;

  while (strp < stre) {
    char cc = *strp;
    /* probably should escape unicode, but that's a future levi problem */
    if ((uint8_t)cc >= ' ' && !(cc == '"' || cc == '\\')) {
      ++strp;
      continue;
    }
    mrb_str_cat(mrb, bufstr, pbeg, strp - pbeg);

    switch (cc) {
    case '\b':
      mrb_str_cat_lit(mrb, bufstr, "\\b");
      break;
    case '\t':
      mrb_str_cat_lit(mrb, bufstr, "\\t");
      break;
    case '\n':
      mrb_str_cat_lit(mrb, bufstr, "\\n");
      break;
    case '\f':
      mrb_str_cat_lit(mrb, bufstr, "\\f");
      break;
    case '\r':
      mrb_str_cat_lit(mrb, bufstr, "\\r");
      break;
    case '"':
      mrb_str_cat_lit(mrb, bufstr, "\\\"");
      break;
    case '\\':
      mrb_str_cat_lit(mrb, bufstr, "\\\\");
      break;
    default:
    }

    ++strp;
    pbeg = strp;
  }

  mrb_str_cat(mrb, bufstr, pbeg, strp - pbeg);
  return mrb_str_cat_lit(mrb, bufstr, "\"");
}

mrb_value dcj_string_write_json(mrb_state *mrb, mrb_value self,
                                mrb_value bufstr) {
  const struct RString *str = RSTRING(self);

  return dcj_write_json_ptrlen_str(mrb, RSTR_PTR(str), RSTR_LEN(str), bufstr,
                                   false);
}

mrb_value dcj_symbol_write_json(mrb_state *mrb, const struct json_opts_t *opts,
                                mrb_value self, mrb_value bufstr,
                                _Bool as_key) {
  mrb_int len;
  const char *name = mrb_sym2name_len(mrb, mrb_symbol(self), &len);

  if (!opts->sym_ext || as_key) {
    return dcj_write_json_ptrlen_str(mrb, name, len, bufstr, opts->string_symbols);
  } else if (opts->string_symbols) {
    return dcj_write_json_ptrlen_str(mrb, name, len, bufstr, true);
  } else {
    if (mrb_nil_p(bufstr)) {
      bufstr = mrb_str_new_capa(mrb, sizeof("{\"@@jm:symbol\":}") + 2 + len);
    }
    mrb_str_cat_lit(mrb, bufstr, "{\"@@jm:symbol\":");
    dcj_write_json_ptrlen_str(mrb, name, len, bufstr, false);
    return mrb_str_cat_lit(mrb, bufstr, "}");
  }
}

mrb_value dcj_array_write_json(mrb_state *mrb, const struct json_opts_t *opts,
                               mrb_value self, mrb_value bufstr) {
  struct RArray *rary = RARRAY(self);
  mrb_int len = ARY_LEN(rary);

  if (len == 0) {
    /* this is terrible! */
    static const char *data[2] = {"[]", "[ ]"};
    static const mrb_int lengths[2] = {2, 3};
    if (mrb_nil_p(bufstr)) {
      return mrb_str_new_static(mrb, data[(uint8_t)opts->spc_nul],
                                lengths[(uint8_t)opts->spc_nul]);
    } else {
      dcj_assert(mrb, mrb_string_p(bufstr), "bufstr not a string");
      return mrb_str_cat(mrb, bufstr, data[(uint8_t)opts->spc_nul],
                         lengths[(uint8_t)opts->spc_nul]);
    }
  }

  if (mrb_nil_p(bufstr)) {
    bufstr = mrb_str_new_capa(mrb, 64);
  }

  if (opts->minify) {
    mrb_str_cat_lit(mrb, bufstr, "[");
    for (mrb_int i = 0; i < ARY_LEN(rary); ++i) {
      if (i != 0) {
        mrb_str_cat_lit(mrb, bufstr, ",");
      }
      dcj_generic_write_json(mrb, opts, ARY_PTR(rary)[i], bufstr, false);
    }
    mrb_str_cat_lit(mrb, bufstr, "]");
  } else {
    struct json_opts_t new_opts = *opts;
    ++new_opts.indent_depth;
    mrb_int indentlen = new_opts.indent_depth * new_opts.indent_width;
    mrb_value indent = mrb_str_new_capa(mrb, indentlen);
    struct RString *rstr = RSTRING(indent);
    memset(RSTR_PTR(rstr), ' ', indentlen);
    mrb_str_cat_lit(mrb, bufstr, "[\n");
    RSTR_SET_LEN(rstr, indentlen);

    for (mrb_int i = 0; i < ARY_LEN(rary); ++i) {
      if (i != 0) {
        mrb_str_cat_lit(mrb, bufstr, ",\n");
      }
      mrb_str_cat_str(mrb, bufstr, indent);
      dcj_generic_write_json(mrb, &new_opts, ARY_PTR(rary)[i], bufstr, false);
    }

    /* again, awful hacks */
    mrb_str_cat_lit(mrb, bufstr, "\n");
    RSTR_SET_LEN(rstr, opts->indent_depth * opts->indent_width);
    mrb_str_cat_str(mrb, bufstr, indent);

    mrb_str_cat_lit(mrb, bufstr, "]");
  }
  return bufstr;
}

struct dcj_hashforeachdata {
  const struct json_opts_t *opts;
  mrb_value bufstr;
  mrb_value indent;
  _Bool first;
};

int dcj_hash_write_pair_minified(mrb_state *mrb, mrb_value key, mrb_value value,
                                 void *ud) {
  struct dcj_hashforeachdata *data = (struct dcj_hashforeachdata *)ud;

  mrb_value bufstr = data->bufstr;
  const struct json_opts_t *opts = data->opts;

  if (!data->first) {
    mrb_str_cat_lit(mrb, bufstr, ",");
  }
  data->first = false;

  if (mrb_symbol_p(key)) {
    dcj_symbol_write_json(mrb, opts, key, bufstr, true);
  } else if (mrb_string_p(key)) {
    dcj_string_write_json(mrb, key, bufstr);
  } else {
    mrb_raisef(mrb, serialization_error_class,
               "key %v is neither a String nor a Symbol (%T)", key, key);
  }

  mrb_str_cat_lit(mrb, bufstr, ":");

  dcj_generic_write_json(mrb, opts, value, bufstr, false);

  return 0;
}

int dcj_hash_write_pair(mrb_state *mrb, mrb_value key, mrb_value value,
                        void *ud) {
  struct dcj_hashforeachdata *data = (struct dcj_hashforeachdata *)ud;

  mrb_value bufstr = data->bufstr;
  const struct json_opts_t *opts = data->opts;

  if (!data->first) {
    mrb_str_cat_lit(mrb, bufstr, ",\n");
  }
  data->first = false;

  mrb_str_cat_str(mrb, bufstr, data->indent);
  if (mrb_symbol_p(key)) {
    dcj_symbol_write_json(mrb, opts, key, bufstr, true);
  } else if (mrb_string_p(key)) {
    dcj_string_write_json(mrb, key, bufstr);
  } else {
    mrb_raisef(mrb, serialization_error_class,
               "key %v is neither a String nor a Symbol (%T)", key, key);
  }

  mrb_str_cat_lit(mrb, bufstr, ": ");

  dcj_generic_write_json(mrb, opts, value, bufstr, false);

  return 0;
}

mrb_value dcj_hash_write_json(mrb_state *mrb, const struct json_opts_t *opts,
                              mrb_value self, mrb_value bufstr) {
  struct RHash *rhsh = RHASH(self);
  mrb_int len = rhsh->size;

  if (len == 0) {
    /* this is terrible! */
    static const char *data[2] = {"{}", "{ }"};
    static const mrb_int lengths[2] = {2, 3};
    if (mrb_nil_p(bufstr)) {
      return mrb_str_new_static(mrb, data[(uint8_t)opts->spc_nul],
                                lengths[(uint8_t)opts->spc_nul]);
    } else {
      dcj_assert(mrb, mrb_string_p(bufstr), "bufstr not a string");
      return mrb_str_cat(mrb, bufstr, data[(uint8_t)opts->spc_nul],
                         lengths[(uint8_t)opts->spc_nul]);
    }
  }

  if (mrb_nil_p(bufstr)) {
    bufstr = mrb_str_new_capa(mrb, 64);
  }

  if (opts->minify) {
    mrb_str_cat_lit(mrb, bufstr, "{");
    mrb_hash_foreach(mrb, rhsh, dcj_hash_write_pair_minified,
                     &(struct dcj_hashforeachdata){.opts = opts,
                                                   .bufstr = bufstr,
                                                   .indent = mrb_nil_value(),
                                                   .first = true});
    mrb_str_cat_lit(mrb, bufstr, "}");
  } else {
    mrb_str_cat_lit(mrb, bufstr, "{\n");

    struct json_opts_t new_opts = *opts;
    ++new_opts.indent_depth;
    mrb_int indentlen = new_opts.indent_depth * new_opts.indent_width;
    mrb_value indent = mrb_str_new_capa(mrb, indentlen);
    struct RString *rstr = RSTRING(indent);
    memset(RSTR_PTR(rstr), ' ', indentlen);
    RSTR_SET_LEN(rstr, indentlen);

    mrb_hash_foreach(mrb, rhsh, dcj_hash_write_pair,
                     &(struct dcj_hashforeachdata){.opts = &new_opts,
                                                   .bufstr = bufstr,
                                                   .indent = indent,
                                                   .first = true});

    mrb_str_cat_lit(mrb, bufstr, "\n");
    RSTR_SET_LEN(rstr, opts->indent_depth * opts->indent_width);
    mrb_str_cat_str(mrb, bufstr, indent);

    mrb_str_cat_lit(mrb, bufstr, "}");
  }

  return bufstr;
}

mrb_value dcj_true_write_json(mrb_state *mrb, mrb_value bufstr) {
  if (mrb_nil_p(bufstr)) {
    return mrb_str_new_lit(mrb, "true");
  }
  return mrb_str_cat_lit(mrb, bufstr, "true");
}

mrb_value dcj_false_write_json(mrb_state *mrb, mrb_value bufstr) {
  if (mrb_nil_p(bufstr)) {
    return mrb_str_new_lit(mrb, "false");
  }
  return mrb_str_cat_lit(mrb, bufstr, "false");
}

mrb_value dcj_nil_write_json(mrb_state *mrb, mrb_value bufstr) {
  if (mrb_nil_p(bufstr)) {
    return mrb_str_new_lit(mrb, "null");
  }
  return mrb_str_cat_lit(mrb, bufstr, "null");
}

mrb_value dcj_generic_write_json(mrb_state *mrb, const struct json_opts_t *opts,
                                 mrb_value self, mrb_value bufstr,
                                 _Bool as_key) {
  switch (mrb_type(self)) {
  case MRB_TT_INTEGER:
    return dcj_integer_write_json(mrb, self, bufstr);
  case MRB_TT_FLOAT:
    return dcj_float_write_json(mrb, self, bufstr);
  case MRB_TT_STRING:
    return dcj_string_write_json(mrb, self, bufstr);
  case MRB_TT_SYMBOL:
    return dcj_symbol_write_json(mrb, opts, self, bufstr, as_key);
  case MRB_TT_ARRAY:
    return dcj_array_write_json(mrb, opts, self, bufstr);
  case MRB_TT_HASH:
    return dcj_hash_write_json(mrb, opts, self, bufstr);
  case MRB_TT_TRUE:
    return dcj_true_write_json(mrb, bufstr);
  case MRB_TT_FALSE:
    if (mrb_nil_p(self))
      return dcj_nil_write_json(mrb, bufstr);
    return dcj_false_write_json(mrb, bufstr);
  case MRB_TT_OBJECT:
    mrb_value opts_v;
    /* terrible terrible hacks */
    if (mrb_get_argc(mrb) == 0) {
      opts_v = default_opts_val;
    } else {
      opts_v = *mrb_get_argv(mrb);
    }

    mrb_value args[2] = {opts_v, bufstr};
    mrb_value ret = mrb_funcall_argv(mrb, self, usym(to_json), 2, args);
    return ret;
  default:
    dcj_bug(mrb, 0, "non-serializable object");
  }
}

mrb_value dcj_integer_write_json_m(mrb_state *mrb, mrb_value self) {
  write_args(mrb, 0);
  return dcj_integer_write_json(mrb, self, bufstr);
}

mrb_value dcj_float_write_json_m(mrb_state *mrb, mrb_value self) {
  write_args(mrb, 0);
  return dcj_float_write_json(mrb, self, bufstr);
}

mrb_value dcj_string_write_json_m(mrb_state *mrb, mrb_value self) {
  const struct RString *str = RSTRING(self);
  write_args(mrb, (size_t)RSTR_LEN(str) + 2);

  return dcj_string_write_json(mrb, self, bufstr);
}

mrb_value dcj_symbol_write_json_m(mrb_state *mrb, mrb_value self) {
  mrb_int len;
  mrb_sym2name_len(mrb, mrb_symbol(self), &len);
  write_args(mrb, (size_t)len + 2);

  return dcj_symbol_write_json(mrb, opts, self, bufstr, false);
}

mrb_value dcj_array_write_json_m(mrb_state *mrb, mrb_value self) {
  write_args(mrb, RARRAY_LEN(self) == 0 ? 2 : 64);

  return dcj_array_write_json(mrb, opts, self, bufstr);
}

mrb_value dcj_hash_write_json_m(mrb_state *mrb, mrb_value self) {
  write_args(mrb, RHASH(self)->size == 0 ? 2 : 128);

  return dcj_hash_write_json(mrb, opts, self, bufstr);
}

mrb_value dcj_true_write_json_m(mrb_state *mrb, mrb_value) {
  write_args(mrb, 4);
  return dcj_true_write_json(mrb, bufstr);
}

mrb_value dcj_false_write_json_m(mrb_state *mrb, mrb_value) {
  write_args(mrb, 5);
  return dcj_true_write_json(mrb, bufstr);
}
mrb_value dcj_nil_write_json_m(mrb_state *mrb, mrb_value) {
  write_args(mrb, 4);
  return dcj_true_write_json(mrb, bufstr);
}

void drb_register_c_extensions_with_api(mrb_state *mrb, struct drb_api_t *) {
#define hexlutsetup(beg, end, off)                                             \
  do {                                                                         \
    static_assert(beg <= end, "");                                                 \
    for (uint8_t i = beg; i <= end; ++i) {                                     \
      hexlut[i] = i + off;                                                     \
    }                                                                          \
  } while (0)

  hexlutsetup('0', '9', -'0');
  hexlutsetup('a', 'f', -'a' + 10);
  hexlutsetup('A', 'F', -'A' + 10);

#define SFUNC(name, def) init_usym2(mrb, name, def);
#include "symbols.inc"
#undef SFUNC

  struct RClass *argonaut_mod = mrb_define_module_id(mrb, usym(Argonaut));
  struct RClass *json_mod =
      mrb_define_module_under_id(mrb, argonaut_mod, usym(JSON));
  mrb_value json_mod_v = mrb_obj_value(json_mod);

  parse_error_class = mrb_define_class_under_id(
      mrb, json_mod, usym(ParseError),
      mrb_exc_get_id(mrb, mrb_intern_lit(mrb, "StandardError")));

  serialization_error_class = mrb_define_class_under_id(
      mrb, json_mod, usym(SerializationError),
      mrb_exc_get_id(mrb, mrb_intern_lit(mrb, "StandardError")));

  assert_failed_class = mrb_define_class_under_id(
      mrb, json_mod, usym(AssertFailed),
      mrb_exc_get_id(mrb, mrb_intern_lit(mrb, "StandardError")));

  json_opts_class = mrb_define_class_under_id(mrb, json_mod, usym(Options),
                                              mrb->object_class);
  MRB_SET_INSTANCE_TT(json_opts_class, MRB_TT_ISTRUCT);
  mrb_value json_opts_class_v = mrb_obj_value(json_opts_class);

  default_opts_val = dcj_json_opts_newc(mrb, default_opts);
  MRB_SET_FROZEN_FLAG(mrb_obj_ptr(default_opts_val));
  minify_opts_val = dcj_json_opts_newc(mrb, minify_opts);
  MRB_SET_FROZEN_FLAG(mrb_obj_ptr(minify_opts_val));

  true_string = mrb_str_new_lit_frozen(mrb, "true");
  mrb_iv_set(mrb, json_mod_v, usym(__true_str), true_string);
  false_string = mrb_str_new_lit_frozen(mrb, "false");
  mrb_iv_set(mrb, json_mod_v, usym(__false_str), false_string);
  null_string = mrb_str_new_lit_frozen(mrb, "null");
  mrb_iv_set(mrb, json_mod_v, usym(__null_str), null_string);

  mrb_define_class_method_id(mrb, json_opts_class, usym(new),
                             dcj_json_opts_new_m, MRB_ARGS_KEY(0, 8));
  mrb_define_module_function_id(mrb, json_opts_class, usym(default),
                                dcj_json_opts_default, MRB_ARGS_NONE());
  mrb_define_module_function_id(mrb, json_opts_class, usym(minified),
                                dcj_json_opts_minify, MRB_ARGS_NONE());
  mrb_const_set(mrb, json_opts_class_v, usym(DEFAULT), default_opts_val);
  mrb_const_set(mrb, json_opts_class_v, usym(MINIFIED), minify_opts_val);
  mrb_define_method_id(mrb, json_opts_class, usym(inspect),
                       dcj_json_opts_inspect, MRB_ARGS_NONE());
  mrb_define_method_id(mrb, json_opts_class, usym(to_s), dcj_json_opts_inspect,
                       MRB_ARGS_NONE());

  mrb_define_method_id(mrb, mrb->string_class, usym(to_json),
                       dcj_string_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->array_class, usym(to_json),
                       dcj_array_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->hash_class, usym(to_json),
                       dcj_hash_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->integer_class, usym(to_json),
                       dcj_integer_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->float_class, usym(to_json),
                       dcj_float_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->true_class, usym(to_json),
                       dcj_true_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->false_class, usym(to_json),
                       dcj_false_write_json_m, MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->nil_class, usym(to_json), dcj_nil_write_json_m,
                       MRB_ARGS_OPT(1));
  mrb_define_method_id(mrb, mrb->symbol_class, usym(to_json),
                       dcj_symbol_write_json_m, MRB_ARGS_OPT(1));

  mrb_define_module_function_id(mrb, json_mod, usym(parse), dcj_parse_json_m,
                                MRB_ARGS_REQ(1));
}
