/*
 * Copyright (c) 1999 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights Reserved.
 * 
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this
 * file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/*
 * Config.h
 *
 * Configuration Manager for lookupd.
 * Written by Marc Majka
 */

#import "Root.h"
#import "LUGlobal.h"
#import "LUArray.h"
#import "LUDictionary.h"
#import <NetInfo/ni_shared.h>

#define configSourceAutomatic 0
#define configSourceDefault   1
#define configSourceNetInfo   2
#define configSourceFile      3

@interface Config : Root
{
	LUArray *cdict;
	BOOL didSetConfig;
	unsigned int source;
	unsigned int initsource;
	ni_shared_handle_t *sourceDomain;
	char *sourcePath;
	char *sourceDomainName;
}

/* Called at startup by lookupd.m */
- (BOOL)setConfigSource:(int)src path:(char *)path domain:(char *)domain;

/* Array of config dictionaries. */
- (LUArray *)config;

/* Caller should release returned dictionaries */
- (LUDictionary *)configGlobal:(LUArray *)c;
- (LUDictionary *)configForCategory:(LUCategory)cat fromConfig:(LUArray *)c;
- (LUDictionary *)configForAgent:(char *)agent fromConfig:(LUArray *)c;
- (LUDictionary *)configForAgent:(char *)agent category:(LUCategory)cat fromConfig:(LUArray *)c;

/* Caller must free returned string */
- (char *)stringForKey:(char *)key dict:(LUDictionary *)dict default:(char *)def;

- (int)intForKey:(char *)key dict:(LUDictionary *)dict default:(int)def;
- (BOOL)boolForKey:(char *)key dict:(LUDictionary *)dict default:(BOOL)def;

@end
