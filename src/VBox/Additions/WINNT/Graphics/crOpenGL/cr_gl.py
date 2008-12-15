print """
/** @file
 * VBox OpenGL chromium functions header
 */

/*
 * Copyright (C) 2006-2007 Sun Microsystems, Inc.
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa
 * Clara, CA 95054 USA or visit http://www.sun.com if you need
 * additional information or have any questions.
 */
"""
# Copyright (c) 2001, Stanford University
# All rights reserved.
#
# See the file LICENSE.txt for information on redistributing this software.

import sys

import apiutil

apiutil.CopyrightC()

print """
/* DO NOT EDIT - THIS FILE GENERATED BY THE cr_gl.py SCRIPT */
#ifndef __CR_GL_H__
#define __CR_GL_H__

#include "chromium.h"
#include "cr_string.h"
#include "cr_version.h"
#include "stub.h"

#ifdef WINDOWS
#pragma warning( disable: 4055 )
#endif

"""


# Extern-like declarations
keys = apiutil.GetAllFunctions(sys.argv[1]+"/APIspec.txt")
for func_name in keys:
	if "Chromium" == apiutil.Category(func_name):
		continue
	if func_name == "BoundsInfoCR":
		continue
	if "GL_chromium" == apiutil.Category(func_name):
		pass #continue

	return_type = apiutil.ReturnType(func_name)
	params = apiutil.Parameters(func_name)

	print "extern %s cr_gl%s( %s );" % (return_type, func_name,
								  apiutil.MakeDeclarationString( params ))

print "#endif /* __CR_GL_H__ */"
