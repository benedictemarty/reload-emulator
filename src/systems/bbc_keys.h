#pragma once

// bbc_keys.h
//
// BBC Micro internal key numbers (row << 4 | column, as returned negated by
// INKEY) and a host ASCII -> key helper.
//
// ## zlib/libpng license
//
// Copyright (c) 2026 bmarty
// This software is provided 'as-is', without any express or implied warranty.
// In no event will the authors be held liable for any damages arising from the
// use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//     1. The origin of this software must not be misrepresented; you must not
//     claim that you wrote the original software. If you use this software in a
//     product, an acknowledgment in the product documentation would be
//     appreciated but is not required.
//     2. Altered source versions must be plainly marked as such, and must not
//     be misrepresented as being the original software.
//     3. This notice may not be removed or altered from any source
//     distribution.

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    BBC_KEY_Shift = 0x00, BBC_KEY_Ctrl = 0x01,
    BBC_KEY_Q = 0x10, BBC_KEY_3 = 0x11, BBC_KEY_4 = 0x12, BBC_KEY_5 = 0x13, BBC_KEY_f4 = 0x14, BBC_KEY_8 = 0x15,
    BBC_KEY_f7 = 0x16, BBC_KEY_Minus = 0x17, BBC_KEY_Caret = 0x18, BBC_KEY_Left = 0x19,
    BBC_KEY_f0 = 0x20, BBC_KEY_W = 0x21, BBC_KEY_E = 0x22, BBC_KEY_T = 0x23, BBC_KEY_7 = 0x24, BBC_KEY_I = 0x25,
    BBC_KEY_9 = 0x26, BBC_KEY_0 = 0x27, BBC_KEY_Underline = 0x28, BBC_KEY_Down = 0x29,
    BBC_KEY_1 = 0x30, BBC_KEY_2 = 0x31, BBC_KEY_D = 0x32, BBC_KEY_R = 0x33, BBC_KEY_6 = 0x34, BBC_KEY_U = 0x35,
    BBC_KEY_O = 0x36, BBC_KEY_P = 0x37, BBC_KEY_LeftSquareBracket = 0x38, BBC_KEY_Up = 0x39,
    BBC_KEY_CapsLock = 0x40, BBC_KEY_A = 0x41, BBC_KEY_X = 0x42, BBC_KEY_F = 0x43, BBC_KEY_Y = 0x44, BBC_KEY_J = 0x45,
    BBC_KEY_K = 0x46, BBC_KEY_At = 0x47, BBC_KEY_Colon = 0x48, BBC_KEY_Return = 0x49,
    BBC_KEY_ShiftLock = 0x50, BBC_KEY_S = 0x51, BBC_KEY_C = 0x52, BBC_KEY_G = 0x53, BBC_KEY_H = 0x54, BBC_KEY_N = 0x55,
    BBC_KEY_L = 0x56, BBC_KEY_Semicolon = 0x57, BBC_KEY_RightSquareBracket = 0x58, BBC_KEY_Delete = 0x59,
    BBC_KEY_Tab = 0x60, BBC_KEY_Z = 0x61, BBC_KEY_Space = 0x62, BBC_KEY_V = 0x63, BBC_KEY_B = 0x64, BBC_KEY_M = 0x65,
    BBC_KEY_Comma = 0x66, BBC_KEY_Stop = 0x67, BBC_KEY_Slash = 0x68, BBC_KEY_Copy = 0x69,
    BBC_KEY_Escape = 0x70, BBC_KEY_f1 = 0x71, BBC_KEY_f2 = 0x72, BBC_KEY_f3 = 0x73, BBC_KEY_f5 = 0x74, BBC_KEY_f6 = 0x75,
    BBC_KEY_f8 = 0x76, BBC_KEY_f9 = 0x77, BBC_KEY_Backslash = 0x78, BBC_KEY_Right = 0x79,
    // Master 128 numeric keypad (columns 10-12)
    BBC_KEY_Keypad4 = 0x7A, BBC_KEY_Keypad5 = 0x7B, BBC_KEY_Keypad2 = 0x7C, BBC_KEY_Keypad0 = 0x6A, BBC_KEY_Keypad1 = 0x6B,
    BBC_KEY_Keypad3 = 0x6C, BBC_KEY_KeypadHash = 0x5A, BBC_KEY_KeypadStar = 0x5B, BBC_KEY_KeypadComma = 0x5C,
    BBC_KEY_KeypadSlash = 0x4A, BBC_KEY_KeypadDelete = 0x4B, BBC_KEY_KeypadStop = 0x4C, BBC_KEY_KeypadPlus = 0x3A,
    BBC_KEY_KeypadMinus = 0x3B, BBC_KEY_KeypadReturn = 0x3C, BBC_KEY_Keypad8 = 0x2A, BBC_KEY_Keypad9 = 0x2B,
    BBC_KEY_Keypad6 = 0x1A, BBC_KEY_Keypad7 = 0x1B,
    BBC_KEY_Break = 0xFF,
};

// ASCII character -> BBC key. Returns -1 if the character has no key;
// *shift is set when SHIFT is needed. Letter case is decided by the MOS from
// the CAPS LOCK state (on after power-up: letters are upper case, shifted or
// not; toggle it with 0x01 = CAPS LOCK key), so letters never set *shift.
static inline int bbc_key_from_ascii(uint8_t c, bool* shift) {
    *shift = false;
    if (c >= 'a' && c <= 'z') {
        c = (uint8_t)(c - 32);
    }
    if (c >= 'A' && c <= 'Z') {
        static const uint8_t letters[26] = {
            BBC_KEY_A, BBC_KEY_B, BBC_KEY_C, BBC_KEY_D, BBC_KEY_E, BBC_KEY_F, BBC_KEY_G, BBC_KEY_H, BBC_KEY_I,
            BBC_KEY_J, BBC_KEY_K, BBC_KEY_L, BBC_KEY_M, BBC_KEY_N, BBC_KEY_O, BBC_KEY_P, BBC_KEY_Q, BBC_KEY_R,
            BBC_KEY_S, BBC_KEY_T, BBC_KEY_U, BBC_KEY_V, BBC_KEY_W, BBC_KEY_X, BBC_KEY_Y, BBC_KEY_Z};
        return letters[c - 'A'];
    }
    switch (c) {
        case '\n': case '\r': return BBC_KEY_Return;
        case ' ': return BBC_KEY_Space;
        case 0x1B: return BBC_KEY_Escape;
        case 0x7F: case 0x08: return BBC_KEY_Delete;
        case '\t': return BBC_KEY_Tab;
        case 0x01: return BBC_KEY_CapsLock;                // ^A toggles CAPS LOCK (test helper)
        case '0': return BBC_KEY_0;
        case '1': return BBC_KEY_1;
        case '2': return BBC_KEY_2;
        case '3': return BBC_KEY_3;
        case '4': return BBC_KEY_4;
        case '5': return BBC_KEY_5;
        case '6': return BBC_KEY_6;
        case '7': return BBC_KEY_7;
        case '8': return BBC_KEY_8;
        case '9': return BBC_KEY_9;
        case '-': return BBC_KEY_Minus;
        case '^': return BBC_KEY_Caret;
        case '_': return BBC_KEY_Underline;
        case '@': return BBC_KEY_At;
        case ':': return BBC_KEY_Colon;
        case ';': return BBC_KEY_Semicolon;
        case '[': return BBC_KEY_LeftSquareBracket;
        case ']': return BBC_KEY_RightSquareBracket;
        case '\\': return BBC_KEY_Backslash;
        case ',': return BBC_KEY_Comma;
        case '.': return BBC_KEY_Stop;
        case '/': return BBC_KEY_Slash;
        default: break;
    }
    *shift = true;
    switch (c) {
        case '!': return BBC_KEY_1;
        case '"': return BBC_KEY_2;
        case '#': return BBC_KEY_3;
        case '$': return BBC_KEY_4;
        case '%': return BBC_KEY_5;
        case '&': return BBC_KEY_6;
        case '\'': return BBC_KEY_7;
        case '(': return BBC_KEY_8;
        case ')': return BBC_KEY_9;
        case '=': return BBC_KEY_Minus;
        case '~': return BBC_KEY_Caret;
        case '*': return BBC_KEY_Colon;
        case '+': return BBC_KEY_Semicolon;
        case '{': return BBC_KEY_LeftSquareBracket;
        case '}': return BBC_KEY_RightSquareBracket;
        case '|': return BBC_KEY_Backslash;
        case '<': return BBC_KEY_Comma;
        case '>': return BBC_KEY_Stop;
        case '?': return BBC_KEY_Slash;
        default: break;
    }
    *shift = false;
    return -1;
}

#ifdef __cplusplus
}
#endif
