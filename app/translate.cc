#include <cstdlib>
#include <cerrno>
#include <cassert>
#include <climits>
#include <cctype>

#include <string>
#include <vector>
#include <map>

#include "logger.h"
#include "timestamp.h"
#include "teletype.h"
#include "translate.h"

struct tty_symbol
{
    int type;
    int symbol;
    std::string name;
    std::string alias;

    std::string qualified_name();
};

static tty_symbol static_symbol_table[] =
{
    /* namespaces */
    { tty_sym_ns,    tty_sym_flag,          "flag"                      },
    { tty_sym_ns,    tty_sym_code,          "code"                      },
    { tty_sym_ns,    tty_sym_oper,          "operator",       "oper"    },
    { tty_sym_ns,    tty_sym_mod,           "modifer",        "mod"     },
    { tty_sym_ns,    tty_sym_char,          "char"                      },
    { tty_sym_ns,    tty_sym_key,           "key"                       },
    { tty_sym_ns,    tty_sym_string,        "string"                    },
    { tty_sym_ns,    tty_sym_int,           "integer",        "int"     },

    /* flags */
    { tty_sym_flag,  tty_flag_DECCKM,       "app_cursor_keys"           },
    { tty_sym_flag,  tty_flag_DECAWM,       "auto_wrap"                 },
    { tty_sym_flag,  tty_flag_DECTCEM,      "cursor_enable"             },
    { tty_sym_flag,  tty_flag_DECAKM,       "alt_keypad_mode"           },
    { tty_sym_flag,  tty_flag_DECBKM,       "backarrow_sends_delete"    },
    { tty_sym_flag,  tty_flag_ATTBC,        "blinking_cursor"           },
    { tty_sym_flag,  tty_flag_XT8BM,        "eight_bit_mode"            },
    { tty_sym_flag,  tty_flag_XTAS,         "alt_screen"                },
    { tty_sym_flag,  tty_flag_XTSC,         "save_cursor"               },
    { tty_sym_flag,  tty_flag_XTBP,         "bracketed_paste"           },

    /* codes */
    { tty_sym_code,  tty_code_csi,          "CSI",                      },
    { tty_sym_code,  tty_code_ss2,          "SS2",                      },
    { tty_sym_code,  tty_code_ss3,          "SS3",                      },

    /* operators */
    { tty_sym_oper,  tty_oper_plus,         "+"                         },
    { tty_sym_oper,  tty_oper_equal,        "="                         },
    { tty_sym_oper,  tty_oper_arrow,        "->"                        },
    { tty_sym_oper,  tty_oper_emit,         "emit"                      },
    { tty_sym_oper,  tty_oper_copy,         "copy"                      },
    { tty_sym_oper,  tty_oper_paste,        "paste"                     },

    /* modifers */
    { tty_sym_mod,   tty_mod_shift,         "shift"                     },
    { tty_sym_mod,   tty_mod_control,       "control",        "ctrl"    },
    { tty_sym_mod,   tty_mod_alt,           "alt",            "option"  },
    { tty_sym_mod,   tty_mod_super,         "super",          "command" },
#if defined __APPLE__
    { tty_sym_mod,   tty_mod_super,         "ctrl_cmd"                  },
#else
    { tty_sym_mod,   tty_mod_control,       "ctrl_cmd"                  },
#endif
    { tty_sym_mod,   tty_mod_capslock,      "capslock",                 },
    { tty_sym_mod,   tty_mod_numlock,       "numlock",                  },

    /* chars */
    { tty_sym_char,  tty_char_NUL,          "NUL",            "^@"      },
    { tty_sym_char,  tty_char_SOH,          "SOH",            "^A"      },
    { tty_sym_char,  tty_char_STX,          "STX",            "^B"      },
    { tty_sym_char,  tty_char_ETX,          "ETX",            "^C"      },
    { tty_sym_char,  tty_char_EOT,          "EOT",            "^D"      },
    { tty_sym_char,  tty_char_ENQ,          "ENQ",            "^E"      },
    { tty_sym_char,  tty_char_ACK,          "ACK",            "^F"      },
    { tty_sym_char,  tty_char_BEL,          "BEL",            "^G"      },
    { tty_sym_char,  tty_char_BS,           "BS",             "^H"      },
    { tty_sym_char,  tty_char_HT,           "HT",             "^I"      },
    { tty_sym_char,  tty_char_LF,           "LF",             "^J"      },
    { tty_sym_char,  tty_char_VT,           "VT",             "^K"      },
    { tty_sym_char,  tty_char_FF,           "FF",             "^L"      },
    { tty_sym_char,  tty_char_CR,           "CR",             "^M"      },
    { tty_sym_char,  tty_char_SO,           "SO",             "^N"      },
    { tty_sym_char,  tty_char_SI,           "SI",             "^O"      },
    { tty_sym_char,  tty_char_DLE,          "DLE",            "^P"      },
    { tty_sym_char,  tty_char_DC1,          "DC1",            "^Q"      },
    { tty_sym_char,  tty_char_DC2,          "DC2",            "^R"      },
    { tty_sym_char,  tty_char_DC3,          "DC3",            "^S"      },
    { tty_sym_char,  tty_char_DC4,          "DC4",            "^T"      },
    { tty_sym_char,  tty_char_NAK,          "NAK",            "^U"      },
    { tty_sym_char,  tty_char_SYN,          "SYN",            "^V"      },
    { tty_sym_char,  tty_char_ETB,          "ETB",            "^W"      },
    { tty_sym_char,  tty_char_CAN,          "CAN",            "^X"      },
    { tty_sym_char,  tty_char_EM,           "EM",             "^Y"      },
    { tty_sym_char,  tty_char_SUB,          "SUB",            "^Z"      },
    { tty_sym_char,  tty_char_ESC,          "ESC",            "^["      },
    { tty_sym_char,  tty_char_FS,           "FS",             "^\\"     },
    { tty_sym_char,  tty_char_GS,           "GS",             "^]"      },
    { tty_sym_char,  tty_char_RS,           "RS",             "^^"      },
    { tty_sym_char,  tty_char_US,           "US",             "^_"      },
    { tty_sym_char,  tty_char_DEL,          "DEL",            "^?"      },

    /* keys */
    { tty_sym_key,   tty_key_space,         "space",          " "       },
    { tty_sym_key,   tty_key_apostrophe,    "apostrophe",     "'"       },
    { tty_sym_key,   tty_key_comma,         "comma",          ","       },
    { tty_sym_key,   tty_key_minus,         "minus",          "-"       },
    { tty_sym_key,   tty_key_period,        "period",         "."       },
    { tty_sym_key,   tty_key_slash,         "slash",          "/"       },
    { tty_sym_key,   tty_key_0,             "digit_0"                   },
    { tty_sym_key,   tty_key_1,             "digit_1"                   },
    { tty_sym_key,   tty_key_2,             "digit_2"                   },
    { tty_sym_key,   tty_key_3,             "digit_3"                   },
    { tty_sym_key,   tty_key_4,             "digit_4"                   },
    { tty_sym_key,   tty_key_5,             "digit_5"                   },
    { tty_sym_key,   tty_key_6,             "digit_6"                   },
    { tty_sym_key,   tty_key_7,             "digit_7"                   },
    { tty_sym_key,   tty_key_8,             "digit_8"                   },
    { tty_sym_key,   tty_key_9,             "digit_9"                   },
    { tty_sym_key,   tty_key_semicolon,     "semicolon",      ";"       },
    { tty_sym_key,   tty_key_equal,         "equal",          "="       },
    { tty_sym_key,   tty_key_a,             "roman_a"                   },
    { tty_sym_key,   tty_key_b,             "roman_b"                   },
    { tty_sym_key,   tty_key_c,             "roman_c"                   },
    { tty_sym_key,   tty_key_d,             "roman_d"                   },
    { tty_sym_key,   tty_key_e,             "roman_e"                   },
    { tty_sym_key,   tty_key_f,             "roman_f"                   },
    { tty_sym_key,   tty_key_g,             "roman_g"                   },
    { tty_sym_key,   tty_key_h,             "roman_h"                   },
    { tty_sym_key,   tty_key_i,             "roman_i"                   },
    { tty_sym_key,   tty_key_j,             "roman_j"                   },
    { tty_sym_key,   tty_key_k,             "roman_k"                   },
    { tty_sym_key,   tty_key_l,             "roman_l"                   },
    { tty_sym_key,   tty_key_m,             "roman_m"                   },
    { tty_sym_key,   tty_key_n,             "roman_n"                   },
    { tty_sym_key,   tty_key_o,             "roman_o"                   },
    { tty_sym_key,   tty_key_p,             "roman_p"                   },
    { tty_sym_key,   tty_key_q,             "roman_q"                   },
    { tty_sym_key,   tty_key_r,             "roman_r"                   },
    { tty_sym_key,   tty_key_s,             "roman_s"                   },
    { tty_sym_key,   tty_key_t,             "roman_t"                   },
    { tty_sym_key,   tty_key_u,             "roman_u"                   },
    { tty_sym_key,   tty_key_v,             "roman_v"                   },
    { tty_sym_key,   tty_key_w,             "roman_w"                   },
    { tty_sym_key,   tty_key_x,             "roman_x"                   },
    { tty_sym_key,   tty_key_y,             "roman_y"                   },
    { tty_sym_key,   tty_key_z,             "roman_z"                   },
    { tty_sym_key,   tty_key_left_bracket,  "left_bracket",   "["       },
    { tty_sym_key,   tty_key_backslash,     "backslash",      "\\"      },
    { tty_sym_key,   tty_key_right_bracket, "right_bracket",  "]"       },
    { tty_sym_key,   tty_key_grave_accent,  "grave_accent",   "`"       },
    { tty_sym_key,   tty_key_world_1,       "world_1"                   },
    { tty_sym_key,   tty_key_world_2,       "world_2"                   },
    { tty_sym_key,   tty_key_escape,        "escape"                    },
    { tty_sym_key,   tty_key_enter,         "enter"                     },
    { tty_sym_key,   tty_key_tab,           "tab"                       },
    { tty_sym_key,   tty_key_backspace,     "backspace"                 },
    { tty_sym_key,   tty_key_insert,        "insert"                    },
    { tty_sym_key,   tty_key_delete,        "delete"                    },
    { tty_sym_key,   tty_key_right,         "right"                     },
    { tty_sym_key,   tty_key_left,          "left"                      },
    { tty_sym_key,   tty_key_down,          "down"                      },
    { tty_sym_key,   tty_key_up,            "up"                        },
    { tty_sym_key,   tty_key_page_up,       "page_up"                   },
    { tty_sym_key,   tty_key_page_down,     "page_down"                 },
    { tty_sym_key,   tty_key_home,          "home"                      },
    { tty_sym_key,   tty_key_end,           "end"                       },
    { tty_sym_key,   tty_key_caps_lock,     "caps_lock"                 },
    { tty_sym_key,   tty_key_scroll_lock,   "scroll_lock"               },
    { tty_sym_key,   tty_key_num_lock,      "num_lock"                  },
    { tty_sym_key,   tty_key_print_screen,  "print_screen"              },
    { tty_sym_key,   tty_key_pause,         "pause"                     },
    { tty_sym_key,   tty_key_f1,            "f1"                        },
    { tty_sym_key,   tty_key_f2,            "f2"                        },
    { tty_sym_key,   tty_key_f3,            "f3"                        },
    { tty_sym_key,   tty_key_f4,            "f4"                        },
    { tty_sym_key,   tty_key_f5,            "f5"                        },
    { tty_sym_key,   tty_key_f6,            "f6"                        },
    { tty_sym_key,   tty_key_f7,            "f7"                        },
    { tty_sym_key,   tty_key_f8,            "f8"                        },
    { tty_sym_key,   tty_key_f9,            "f9"                        },
    { tty_sym_key,   tty_key_f10,           "f10"                       },
    { tty_sym_key,   tty_key_f11,           "f11"                       },
    { tty_sym_key,   tty_key_f12,           "f12"                       },
    { tty_sym_key,   tty_key_f13,           "f13"                       },
    { tty_sym_key,   tty_key_f14,           "f14"                       },
    { tty_sym_key,   tty_key_f15,           "f15"                       },
    { tty_sym_key,   tty_key_f16,           "f16"                       },
    { tty_sym_key,   tty_key_f17,           "f17"                       },
    { tty_sym_key,   tty_key_f18,           "f18"                       },
    { tty_sym_key,   tty_key_f19,           "f19"                       },
    { tty_sym_key,   tty_key_f20,           "f20"                       },
    { tty_sym_key,   tty_key_f21,           "f21"                       },
    { tty_sym_key,   tty_key_f22,           "f22"                       },
    { tty_sym_key,   tty_key_f23,           "f23"                       },
    { tty_sym_key,   tty_key_f24,           "f24"                       },
    { tty_sym_key,   tty_key_f25,           "f25"                       },
    { tty_sym_key,   tty_key_pad_0,         "keypad_0"                  },
    { tty_sym_key,   tty_key_pad_1,         "keypad_1"                  },
    { tty_sym_key,   tty_key_pad_2,         "keypad_2"                  },
    { tty_sym_key,   tty_key_pad_3,         "keypad_3"                  },
    { tty_sym_key,   tty_key_pad_4,         "keypad_4"                  },
    { tty_sym_key,   tty_key_pad_5,         "keypad_5"                  },
    { tty_sym_key,   tty_key_pad_6,         "keypad_6"                  },
    { tty_sym_key,   tty_key_pad_7,         "keypad_7"                  },
    { tty_sym_key,   tty_key_pad_8,         "keypad_8"                  },
    { tty_sym_key,   tty_key_pad_9,         "keypad_9"                  },
    { tty_sym_key,   tty_key_pad_decimal,   "keypad_decimal"            },
    { tty_sym_key,   tty_key_pad_divide,    "keypad_divide"             },
    { tty_sym_key,   tty_key_pad_multiply,  "keypad_multiply"           },
    { tty_sym_key,   tty_key_pad_subtract,  "keypad_subtract"           },
    { tty_sym_key,   tty_key_pad_add,       "keypad_add"                },
    { tty_sym_key,   tty_key_pad_enter,     "keypad_enter"              },
    { tty_sym_key,   tty_key_pad_equal,     "keypad_equal"              },
    { tty_sym_key,   tty_key_left_shift,    "left_shift"                },
    { tty_sym_key,   tty_key_left_control,  "left_control"              },
    { tty_sym_key,   tty_key_left_alt,      "left_alt"                  },
    { tty_sym_key,   tty_key_left_super,    "left_super"                },
    { tty_sym_key,   tty_key_right_shift,   "right_shift"               },
    { tty_sym_key,   tty_key_right_control, "right_control"             },
    { tty_sym_key,   tty_key_right_alt,     "right_alt"                 },
    { tty_sym_key,   tty_key_right_super,   "right_super"               },
    { tty_sym_key,   tty_key_menu,          "menu"                      },
};

struct tty_keymap
{
    std::vector<tty_symbol> symbol;
    std::multimap<int,size_t> symbol_symbol;
    std::multimap<std::string,size_t> name_symbol;
    std::vector<std::vector<size_t>> clause;
    std::multimap<int,size_t> key_clause;

    enum state {
        state_whitespace,
        state_comment,
        state_identifier,
        state_punctuation,
        state_integer,
        state_string,
        state_string_escape,
        state_eol
    };

    tty_keymap();

    void index_symbols();
    size_t insert_symbol(tty_symbol sym);
    size_t lookup_symbol(tty_sym type, std::string name);
    std::string lookup_name(tty_sym type, int symbol);
    void parse_map(std::vector<char> &input);
    void verify_map();
    void index_map();
    void dump_map();
    bool check(size_t idx, std::vector<tty_keypress> seq, int flags);
    size_t match(std::vector<tty_keypress> seq, int flags);
    tty_translate_result translate(size_t clause_idx);
};

static std::string format_string(const char* fmt, ...)
{
    std::vector<char> buf;
    va_list args1, args2;
    int len, ret;

    va_start(args1, fmt);
    len = vsnprintf(NULL, 0, fmt, args1);
    assert(len >= 0);
    va_end(args1);

    buf.resize(len + 1);
    va_start(args2, fmt);
    ret = vsnprintf(buf.data(), buf.capacity(), fmt, args2);
    assert(len == ret);
    va_end(args2);

    return std::string(buf.data(), len);
}

std::string tty_symbol::qualified_name()
{
    switch (type) {
    case tty_sym_ns: return format_string("ns.%s", name.c_str());
    case tty_sym_flag: return format_string("flag.%s", name.c_str());
    case tty_sym_code: return format_string("code.%s", name.c_str());
    case tty_sym_oper: return format_string("operator.%s", name.c_str());
    case tty_sym_mod: return format_string("modifer.%s", name.c_str());
    case tty_sym_char: return format_string("char.%s", name.c_str());
    case tty_sym_key: return format_string("key.%s", name.c_str());
    case tty_sym_string: return format_string("string(\"%s\")", name.c_str());
    case tty_sym_int: return format_string("integer(\"%d\")", symbol);
    }
    return format_string("unknown.%s", name.c_str());
}

tty_keymap::tty_keymap()
{
    for (auto &sym : static_symbol_table) {
        insert_symbol(sym);
    }
}

size_t tty_keymap::insert_symbol(tty_symbol sym)
{
    size_t idx = symbol.size();
    symbol.push_back(sym);
    symbol_symbol.insert({ sym.symbol, idx });
    if (sym.name.size() > 0) {
        name_symbol.insert({ std::string(sym.name), idx });
    }
    if (sym.alias.size() > 0) {
        name_symbol.insert({ std::string(sym.alias), idx });
    }
    return idx;
}

std::string tty_keymap::lookup_name(tty_sym type, int sym)
{
    auto i = symbol_symbol.lower_bound(sym);
    while (i != symbol_symbol.end() && i->first == sym) {
        if (type == symbol[i->second].type ||
            (type == tty_sym_any && symbol[i->second].type != tty_sym_string)) {
            return symbol[i->second].name;
        }
        i++;
    }
    return "unknown";
}

size_t tty_keymap::lookup_symbol(tty_sym type, std::string name)
{
    auto i = name_symbol.lower_bound(name);
    while (i != name_symbol.end() && i->first == name) {
        if (type == symbol[i->second].type ||
            (type == tty_sym_any && symbol[i->second].type != tty_sym_string)) {
            return i->second;
        }
        i++;
    }
    return (size_t)-1ll;
}

void tty_keymap::parse_map(std::vector<char> &input)
{
    size_t line = 1, offset = 0, length = input.size();
    std::string current;
    enum state s = state_whitespace;
    int string_number = 0;

    clause.clear();
    clause.push_back(std::vector<size_t>());

    while (offset < length)
    {
        char c = input[offset++];
        switch (s)
        {
        case state_whitespace:
            if (c == '\n') {
                line++;
            } else if (c == '#') {
                s = state_comment;
            } else if (c == ';') {
                s = state_eol;
            } else if (c == '"') {
                s = state_string;
            } else if (isspace(c)) {

            } else {
                s = state_identifier;
                offset--; // reprocess char
            }
            break;

        case state_comment:
            if (c == '\n') {
                s = state_whitespace;
                line++;
            }
            break;

        case state_identifier:
            if (isdigit(c) && current.size() == 0) {
                s = state_integer;
                offset--; // reprocess char
                break;
            } else if (isspace(c)) {
                s = state_whitespace;
            } else if (c == '#') {
                s = state_comment;
            } else if (c == ';') {
                s = state_eol;
            } else if (isalpha(c) || isdigit(c) || c == '_') {
                current.append(1, c);
            } else {
                s = state_punctuation;
                offset--; // reprocess char
            }
            if (s != state_identifier && current.size() > 0) {
                size_t idx = lookup_symbol(tty_sym_any, current);
                if (idx >= symbol.size()) {
                    Error("keymap parse line %d: unknown token %s\n",
                        line, current.c_str());
                } else {
                    clause.back().push_back(idx);
                }
                current.clear();
            }
            break;

        case state_punctuation:
            if (isalpha(c) || isdigit(c) || c == '_') {
                s = state_identifier;
                offset--; // reprocess char
            } else if (isspace(c)) {
                s = state_whitespace;
            } else if (c == '#') {
                s = state_comment;
            } else if (c == ';') {
                s = state_eol;
            } else {
                current.append(1, c);
            }
            if (s != state_punctuation) {
                size_t idx = lookup_symbol(tty_sym_oper, current);
                if (idx >= symbol.size() || symbol[idx].type == tty_sym_string) {
                    Error("keymap parse line %d: unknown operator %s\n",
                        line, current.c_str());
                } else {
                    clause.back().push_back(idx);
                }
                current.clear();
            }
            break;

        case state_integer:
            if (isdigit(c)) {
                current.append(1, c);
            } else if (isspace(c)) {
                s = state_whitespace;
            } else if (c == '#') {
                s = state_comment;
            } else if (c == ';') {
                s = state_eol;
            } else {
                s = state_identifier;
                offset--; // reprocess char
            }
            if (s != state_integer) {
                size_t idx = insert_symbol(tty_symbol{
                    tty_sym_int, atoi(current.c_str())
                });
                clause.back().push_back(idx);
                current.clear();
            }
            break;

        case state_string:
            if (c == '"') {
                size_t idx = lookup_symbol(tty_sym_string, current);
                if (idx >= symbol.size()) {
                    idx = insert_symbol(tty_symbol{
                        tty_sym_string, string_number++, current
                    });
                }
                clause.back().push_back(idx);
                current.clear();
                s = state_whitespace;
            } else if (c == '\\') {
                s = state_string_escape;
            } else {
                current.append(1, c);
            }
            break;

        case state_string_escape:
            current.append(1, c);
            s = state_string;
            break;

        case state_eol:
            if (clause.back().size() > 0) {
                clause.push_back(std::vector<size_t>());
            }
            s = state_whitespace;
            break;
        }
    }
    if (clause.back().size() != 0) {
        Error("keymap parse last line unterminated, ignoring");
    }
    clause.pop_back();
}

void tty_keymap::verify_map()
{
    /* [ modifier + ]* key -> emit (code | string)* */
    for (size_t i = 0; i < clause.size(); i++) {
        for (size_t j = 0; j < clause[i].size(); j++) {
            // todo
        }
    }
}

void tty_keymap::index_map()
{
    /* index by the first key */
    for (size_t i = 0; i < clause.size(); i++) {
        for (size_t j = 0; j < clause[i].size(); j++) {
            tty_symbol &sym = symbol[clause[i][j]];
            if (sym.type == tty_sym_key) {
                key_clause.insert({ sym.symbol, i });
                break;
            }
        }
    }
}

void tty_keymap::dump_map()
{
    for (size_t i = 0; i < clause.size(); i++) {
        std::string s;
        for (size_t j = 0; j < clause[i].size(); j++) {
            tty_symbol &sym = symbol[clause[i][j]];
            if (j != 0) s.append(" ");
            s.append(sym.qualified_name());
        }
        printf("%s\n", s.c_str());
    }
}

bool tty_keymap::check(size_t idx, std::vector<tty_keypress> seq, int flags)
{
    int check_flag = 0, check_mods = 0;
    size_t key_idx = 0, checked = 0, matched = 0;

    enum { s_begin, s_flag, s_flagval, s_plus, s_key, s_map, s_emit, s_done } s = s_begin;
    const char * state_names[] = { "begin", "flag", "flagval", "plus", "key", "map", "emit", "done" }; 

    for (size_t sym_idx : clause[idx]) {
        tty_symbol &sym = symbol[sym_idx];
        switch (s) {
        case s_begin:
            // flag or mod or key
            switch (sym.type) {
            case tty_sym_flag:
                check_flag = sym.symbol;
                s = s_flag;
                break;
            case tty_sym_mod:
                check_mods |= sym.symbol;
                s = s_plus;
                break;
            case tty_sym_key:
                if (key_idx >= seq.size()) {
                    Trace("keymap check clause=%zu state=%s key count mismatch",
                        idx, state_names[s]);
                    return false;
                }
                matched += (seq[key_idx].mods == check_mods &&
                            seq[key_idx].key == sym.symbol);
                checked++;
                key_idx++;
                check_mods = 0;
                s = s_key;
                break;
            default:
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_flag:
            // oper.equal
            if (sym.type == tty_sym_oper &&
                sym.symbol == tty_oper_equal)
            {
                s = s_flagval;
            } else {
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_flagval:
            // integer
            if (sym.type == tty_sym_int)
            {
                matched += (((check_flag & flags) == check_flag) == sym.symbol);
                checked++;
                s = s_begin;
            } else {
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_plus:
            // oper.plus
            if (sym.type == tty_sym_oper &&
                sym.symbol == tty_oper_plus) {
                s = s_key;
            } else {
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_key:
            // mod or key or oper.->
            switch (sym.type) {
            case tty_sym_mod:
                check_mods |= sym.symbol;
                s = s_plus;
                break;
            case tty_sym_key:
                if (key_idx >= seq.size()) {
                    Trace("keymap check clause=%zu state=%s key count mismatch",
                        idx, state_names[s]);
                    return false;
                }
                matched += (seq[key_idx].mods == check_mods &&
                            seq[key_idx].key == sym.symbol);
                checked++;
                key_idx++;
                check_mods = 0;
                s = s_key;
                break;
            case tty_sym_oper:
                if (key_idx != seq.size()) {
                    Trace("keymap check clause=%zu state=%s key count mismatch",
                        idx, state_names[s]);
                    return false;
                }
                if (sym.symbol == tty_oper_arrow) {
                    s = s_map;
                } else {
                    Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                        idx, state_names[s], sym.qualified_name().c_str());
                    return false;
                }
                break;
            default:
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_map:
            // emit
            if (sym.type == tty_sym_oper && sym.symbol == tty_oper_emit) {
                s = s_emit;
            } else if (sym.type == tty_sym_oper && sym.symbol == tty_oper_copy) {
                s = s_done;
            } else if (sym.type == tty_sym_oper && sym.symbol == tty_oper_paste) {
                s = s_done;
            } else {
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_emit:
            // code or char or string
            switch (sym.type) {
            case tty_sym_code:
            case tty_sym_char:
            case tty_sym_string:
                break;
            default:
                Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                    idx, state_names[s], sym.qualified_name().c_str());
                return false;
            }
            break;
        case s_done:
            Error("keymap check clause=%zu state=%s unexpected symbol %s\n",
                idx, state_names[s], sym.qualified_name().c_str());
            return false;
        }
    }

    return (checked == matched);
}

size_t tty_keymap::match(std::vector<tty_keypress> seq, int flags)
{
    if (seq.size() == 0) {
        return (size_t)-1ll;
    }

    auto i = key_clause.lower_bound(seq.front().key);
    while (i != key_clause.end() && i->first == seq.front().key) {
        if (check(i->second, seq, flags)) return i->second;
        i++;
    }

    return (size_t)-1ll;
}

tty_translate_result tty_keymap::translate(size_t clause_idx)
{
    if (clause_idx >= clause.size()) {
        return { tty_oper_none, std::string() };
    }

    std::string data;
    bool found_emit = false;
    for (size_t sym_idx : clause[clause_idx]) {
        tty_symbol &sym = symbol[sym_idx];
        if (sym.type == tty_sym_oper && sym.symbol == tty_oper_copy) {
            return { tty_oper_copy, std::string() };
        } else if (sym.type == tty_sym_oper && sym.symbol == tty_oper_paste) {
            return { tty_oper_paste, std::string() };
        } else if (sym.type == tty_sym_oper && sym.symbol == tty_oper_emit) {
            found_emit = true;
        } else if (found_emit) {
            switch (sym.type) {
            case tty_sym_code:
                switch (sym.symbol) {
                case tty_code_csi: data.append("\x1b["); break;
                case tty_code_ss2: data.append("\x1bN"); break;
                case tty_code_ss3: data.append("\x1bO"); break;
                }
                break;
            case tty_sym_char:
                data.append(1, sym.symbol);
                break;
            case tty_sym_string:
                data.append(sym.name);
                break;
            }
        }
    }

    return { found_emit ? tty_oper_emit : tty_oper_none, data };
}

static tty_keymap km;

void tty_keymap_init(std::vector<char> &input)
{
    km.parse_map(input);
    km.index_map();
}

void tty_keymap_dump()
{
    km.dump_map();
}

tty_translate_result tty_keymap_translate(std::vector<tty_keypress> seq, int flags)
{
    return km.translate(km.match(seq, flags));
}

std::string tty_keymap_mod_name(int mod)
{
    return km.lookup_name(tty_sym_mod, mod);
}

std::string tty_keymap_key_name(int key)
{
    return km.lookup_name(tty_sym_key, key);
}
