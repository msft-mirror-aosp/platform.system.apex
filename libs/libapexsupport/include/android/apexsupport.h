// Copyright 2023, The Android Open Source Project
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <stdint.h>
#include <sys/cdefs.h>

__BEGIN_DECLS

/**
 * AApexInfo represents an information object for an APEX including name
 * and version.
 */
struct AApexInfo;
typedef struct AApexInfo AApexInfo;

/**
 * AApexInfoError tells the error when AApexInfo_create() fails.
 */
typedef enum AApexInfoError : int32_t {
  /* No error */
  AAPEXINFO_OK,
  /* Out parameter is null. */
  AAPEXINFO_NULL,
  /* The calling process is not from an APEX. */
  AAPEXINFO_NO_APEX,
  /* Failed to get the exectuable path of the calling process.
   * See the log for details.
   */
  AAPEXINFO_ERROR_EXECUTABLE_PATH,
  /* The current APEX is ill-formed. eg) No/invalid apex_manifest.pb.
   * See the log for details.
   */
  AAPEXINFO_INVALID_APEX,
} AApexInfoError;

/**
 * Creates an AApexInfo object from the current calling executable. For example,
 * when called by a binary from /apex/com.android.foo/bin/foo, this will set an
 * out parameter with an AApexInfo object corresponding the APEX
 * "com.android.foo". The allocated AApexInfo object has to be deallocated using
 * AApexInfo_destroy().
 *
 * \param info out parameter for an AApexInfo object for the current APEX
 *
 * \returns AApexInfoError
 */
__attribute__((warn_unused_result)) AApexInfoError
AApexInfo_create(AApexInfo **info) __INTRODUCED_IN(__ANDROID_API_V__);

/**
 * Destroys an AApexInfo object created by AApexInfo_create().
 *
 * \param info pointer to the AApexInfo object created by AApexInfo_create()
 */
void AApexInfo_destroy(AApexInfo *info) __INTRODUCED_IN(__ANDROID_API_V__);

/**
 * Returns a C-string for the APEX name.
 *
 * NOTE: The lifetime of the returned C-string is bound to the AApexInfo object.
 * So, it has to be copied if it needs to be alive even after AApexInfo_destroy
 * is called.
 *
 * \param info pointer to the AApexInfo object created by AApexInfo_create()
 *
 * \return the APEX name. null if AApexInfo is null.
 */
__attribute__((warn_unused_result)) char *
AApexInfo_getName(const AApexInfo *info) __INTRODUCED_IN(__ANDROID_API_V__);

/**
 * Returns the APEX version.
 *
 * \param info pointer to the AApexInfo object created by AApexInfo_create()
 *
 * \return the APEX version. -1 if AApexInfo is null.
 */
int64_t AApexInfo_getVersion(const AApexInfo *info)
    __INTRODUCED_IN(__ANDROID_API_V__);

__END_DECLS
