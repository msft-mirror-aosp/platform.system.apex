/*
 * Copyright (C) 2022 The Android Open Source Project
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

package com.android.tests.apex.app;

import static com.google.common.truth.Truth.assertThat;

import static org.junit.Assert.assertTrue;
import static org.junit.Assert.fail;

import android.Manifest;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.os.SystemProperties;

import androidx.test.platform.app.InstrumentationRegistry;

import com.android.cts.install.lib.Install;
import com.android.cts.install.lib.InstallUtils;
import com.android.cts.install.lib.TestApp;

import org.junit.Test;
import org.junit.runner.RunWith;
import org.junit.runners.JUnit4;

import java.nio.file.Files;
import java.nio.file.Path;
import java.nio.file.Paths;
import java.util.function.Supplier;

@RunWith(JUnit4.class)
public class VendorApexTests {

    private static final String TAG = "VendorApexTests";

    private static final String APEX_PACKAGE_NAME = "com.android.apex.vendor.foo";
    private static final TestApp Apex2Rebootless = new TestApp(
            "com.android.apex.vendor.foo.v2", APEX_PACKAGE_NAME, 2,
            /*isApex*/true, "com.android.apex.vendor.foo.v2.apex");
    private static final TestApp Apex2RequireNativeLibs = new TestApp(
            "com.android.apex.vendor.foo.v2_with_requireNativeLibs", APEX_PACKAGE_NAME, 2,
            /*isApex*/true, "com.android.apex.vendor.foo.v2_with_requireNativeLibs.apex");
    private static final TestApp Apex2Service = new TestApp(
            "com.android.apex.vendor.foo.v2_with_service", APEX_PACKAGE_NAME, 2,
            /*isApex*/true, "com.android.apex.vendor.foo.v2_with_service.apex");

    @Test
    public void testRebootlessUpdate() throws Exception {
        InstallUtils.dropShellPermissionIdentity();
        InstallUtils.adoptShellPermissionIdentity(Manifest.permission.INSTALL_PACKAGE_UPDATES);

        final PackageManager pm =
                InstrumentationRegistry.getInstrumentation().getContext().getPackageManager();
        {
            PackageInfo apex = pm.getPackageInfo(APEX_PACKAGE_NAME, PackageManager.MATCH_APEX);
            assertThat(apex.getLongVersionCode()).isEqualTo(1);
            assertThat(apex.applicationInfo.sourceDir).startsWith("/vendor/apex");
        }

        Install.single(Apex2Rebootless).commit();

        {
            PackageInfo apex = pm.getPackageInfo(APEX_PACKAGE_NAME, PackageManager.MATCH_APEX);
            assertThat(apex.getLongVersionCode()).isEqualTo(2);
            assertThat(apex.applicationInfo.sourceDir).startsWith("/data/apex/active");
        }
    }

    @Test
    public void testGenerateLinkerConfigurationOnUpdate() throws Exception {
        InstallUtils.dropShellPermissionIdentity();
        InstallUtils.adoptShellPermissionIdentity(Manifest.permission.INSTALL_PACKAGE_UPDATES);

        // There's no ld.config.txt for v1 (preinstalled, empty)
        final Path ldConfigTxt = Paths.get("/linkerconfig", APEX_PACKAGE_NAME, "ld.config.txt");
        assertTrue(Files.notExists(ldConfigTxt));

        Install.single(Apex2RequireNativeLibs).commit();

        // v2 uses "libbinder_ndk.so" (requireNativeLibs)
        assertTrue(Files.exists(ldConfigTxt));
        assertThat(Files.readAllLines(ldConfigTxt))
            .contains("namespace.default.link.system.shared_libs += libbinder_ndk.so");
    }

    @Test
    public void testRestartServiceAfterRebootlessUpdate() throws Exception {
        InstallUtils.dropShellPermissionIdentity();
        InstallUtils.adoptShellPermissionIdentity(Manifest.permission.INSTALL_PACKAGE_UPDATES);

        assertThat(SystemProperties.get("init.svc.apex_vendor_foo_v1", ""))
            .isEqualTo("running");
        assertThat(SystemProperties.get("init.svc.apex_vendor_foo_v2", ""))
            .isEqualTo("");

        Install.single(Apex2Service).commit();

        assertThat(SystemProperties.get("init.svc.apex_vendor_foo_v1", ""))
            .isEqualTo("stopped");
        assertThat(SystemProperties.get("apex." + APEX_PACKAGE_NAME + ".ready", ""))
            .isEqualTo("true");
        assertAwait(
                () -> SystemProperties.get("init.svc.apex_vendor_foo_v2", "").equals("running"),
                5000,
                "v2 not started");
    }

    private static void assertAwait(Supplier<Boolean> test, long millis, String failMessage)
            throws Exception {
        long start = System.currentTimeMillis();
        do {
            if (test.get()) {
                return;
            }
            if (System.currentTimeMillis() - start > millis) {
                fail(failMessage);
                return;
            }
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                // nothing to do
            }
        } while (true);
    }
}
