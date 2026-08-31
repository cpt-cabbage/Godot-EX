/**************************************************************************/
/*  ocio_firewall.h                                                       */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#pragma once

// The exception firewall between OpenColorIO and the rest of Godot.
//
// OCIO reports every error by throwing OCIO::Exception, but Godot is built with
// -fno-exceptions. An exception must never unwind through a frame compiled
// without exception support, so the boundary cannot be a single wrapper
// function called from ordinary Godot code: it has to be the translation unit.
// Everything under modules/ocio/backend/ is compiled with exceptions enabled
// (see the SCsub) and is the only code permitted to include an OCIO header or
// to write `try` / `catch`. Those files wrap each entry point in OCIO_GUARD and
// return plain Error codes, so callers outside this directory stay
// exception-free and never see an OCIO type.

#include "core/error/error_list.h"
#include "core/error/error_macros.h"
#include "core/string/ustring.h"

#include <exception>

// Runs `m_body` and converts any exception into an Error return.
//
// On failure the message is written to `r_error` when one was supplied, and
// always logged, since a silently mis-loaded config would otherwise show up
// only as wrong colour on screen. Use inside a function returning Error.
#define OCIO_GUARD(m_error_ptr, m_body)                                              \
	try {                                                                            \
		m_body                                                                       \
	} catch (const std::exception &m_exception) {                                    \
		const String m_message = String::utf8(m_exception.what());                   \
		if (m_error_ptr) {                                                           \
			*(m_error_ptr) = m_message;                                              \
		}                                                                            \
		ERR_PRINT(vformat("OpenColorIO: %s", m_message));                            \
		return ERR_INVALID_DATA;                                                     \
	} catch (...) {                                                                  \
		const String m_message = "unknown error";                                    \
		if (m_error_ptr) {                                                           \
			*(m_error_ptr) = m_message;                                              \
		}                                                                            \
		ERR_PRINT("OpenColorIO: unknown error.");                                    \
		return ERR_INVALID_DATA;                                                     \
	}                                                                                \
	((void)0)

// Same, for functions that cannot report an Error and must fall back to a value.
#define OCIO_GUARD_V(m_fallback, m_body)                                             \
	try {                                                                            \
		m_body                                                                       \
	} catch (const std::exception &m_exception) {                                    \
		ERR_PRINT(vformat("OpenColorIO: %s", String::utf8(m_exception.what())));      \
		return m_fallback;                                                           \
	} catch (...) {                                                                  \
		ERR_PRINT("OpenColorIO: unknown error.");                                    \
		return m_fallback;                                                           \
	}                                                                                \
	((void)0)
