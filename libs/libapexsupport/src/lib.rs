/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//! A FFI wrapper for APEX support library

mod apexinfo;

use apexinfo::{AApexInfo, AApexInfoError};
use std::ffi::c_char;

/// NOTE: Keep these constants in sync with apexsupport.h
const AAPEXINFO_OK: i32 = 0;
const AAPEXINFO_NULL: i32 = 1;
const AAPEXINFO_NO_APEX: i32 = 2;
const AAPEXINFO_ERROR_GET_EXECUTABLE_PATH: i32 = 3;
const AAPEXINFO_INALID_APEX: i32 = 4;

fn as_error_code(err: &AApexInfoError) -> i32 {
    match err {
        AApexInfoError::PathNotFromApex(_) => AAPEXINFO_NO_APEX,
        AApexInfoError::ExePathUnavailable(_) => AAPEXINFO_ERROR_GET_EXECUTABLE_PATH,
        AApexInfoError::InvalidApex(_) => AAPEXINFO_INALID_APEX,
    }
}

#[no_mangle]
/// Creates AApexInfo object when called by the executable from an APEX
///
/// # Safety
///
/// The provided pointer must be a valid object.
pub unsafe extern "C" fn AApexInfo_create(out: *mut *mut AApexInfo) -> i32 {
    if out.is_null() {
        // TODO(b/271488212): Use Rust logger.
        eprintln!("AApexInfo_create() is called with nullptr.");
        return AAPEXINFO_NULL;
    }
    let out = unsafe { out.as_mut().unwrap() };
    match AApexInfo::create() {
        Ok(info) => {
            *out = Box::into_raw(Box::new(info));
            AAPEXINFO_OK
        }
        Err(err) => {
            // TODO(b/271488212): Use Rust logger.
            eprintln!("AApexInfo_create(): {err:?}");
            as_error_code(&err)
        }
    }
}

#[no_mangle]
/// Destroys AApexInfo object created by AApexInfo_create(). No-op when called
/// with null pointer.
///
/// # Safety
///
/// The provided pointer must point to a valid object or null pointer.
pub unsafe extern "C" fn AApexInfo_destroy(info: *mut AApexInfo) {
    if info.is_null() {
        return;
    }
    unsafe { drop(Box::from_raw(info)) };
}

#[no_mangle]
/// Returns a C-string for APEX name. Nullptr if null is given as AApexInfo.
///
/// # Safety
///
/// The provided pointer must point to a valid object or null pointer.
pub unsafe extern "C" fn AApexInfo_getName(info: *const AApexInfo) -> *const c_char {
    if info.is_null() {
        return std::ptr::null();
    }
    let info = unsafe { &*info };
    info.name.as_ptr()
}

#[no_mangle]
/// Returns a version of the APEX. -1 if null is given as AApexInfo.
///
/// # Safety
///
/// The provided pointer must point to a valid object or null pointer.
pub unsafe extern "C" fn AApexInfo_getVersion(info: *const AApexInfo) -> i64 {
    if info.is_null() {
        return -1;
    }
    let info = unsafe { &*info };
    info.version
}
