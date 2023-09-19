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

package android.net.thread;

import static java.util.Objects.requireNonNull;

import android.annotation.NonNull;
import android.annotation.SystemApi;
import android.annotation.SystemService;
import android.content.Context;
import android.os.RemoteException;

import com.android.net.module.util.CollectionUtils;

import java.util.Collections;
import java.util.List;

/**
 * Provides the primary API for managing app aspects of Thread network connectivity.
 *
 * @hide
 */
@SystemApi
@SystemService(ThreadNetworkManager.SERVICE_NAME)
public class ThreadNetworkManager {
    /**
     * This value tracks {@link Context#THREAD_NETWORK_SERVICE}.
     *
     * <p>This is needed because at the time this service is created, it needs to support both
     * Android U and V but {@link Context#THREAD_NETWORK_SERVICE} Is only available on the V branch.
     *
     * <p>Note that this is not added to NetworkStack ConstantsShim because we need this constant in
     * the framework library while ConstantsShim is only linked against the service library.
     *
     * @hide
     */
    public static final String SERVICE_NAME = "thread_network";

    /**
     * This value tracks {@link PackageManager#FEATURE_THREAD_NETWORK}.
     *
     * <p>This is needed because at the time this service is created, it needs to support both
     * Android U and V but {@link PackageManager#FEATURE_THREAD_NETWORK} Is only available on the V
     * branch.
     *
     * <p>Note that this is not added to NetworkStack COnstantsShim because we need this constant in
     * the framework library while ConstantsShim is only linked against the service library.
     *
     * @hide
     */
    public static final String FEATURE_NAME = "android.hardware.thread_network";

    @NonNull private final Context mContext;
    @NonNull private final List<ThreadNetworkController> mUnmodifiableControllerServices;

    /**
     * Creates a new ThreadNetworkManager instance.
     *
     * @hide
     */
    public ThreadNetworkManager(
            @NonNull Context context, @NonNull IThreadNetworkManager managerService) {
        this(context, makeControllers(managerService));
    }

    private static List<ThreadNetworkController> makeControllers(
            @NonNull IThreadNetworkManager managerService) {
        requireNonNull(managerService, "managerService cannot be null");

        List<IThreadNetworkController> controllerServices;

        try {
            controllerServices = managerService.getAllThreadNetworkControllers();
        } catch (RemoteException e) {
            e.rethrowFromSystemServer();
            return Collections.emptyList();
        }

        return CollectionUtils.map(controllerServices, ThreadNetworkController::new);
    }

    private ThreadNetworkManager(
            @NonNull Context context, @NonNull List<ThreadNetworkController> controllerServices) {
        mContext = context;
        mUnmodifiableControllerServices = Collections.unmodifiableList(controllerServices);
    }

    /** Returns the {@link ThreadNetworkController} object of all Thread networks. */
    @NonNull
    public List<ThreadNetworkController> getAllThreadNetworkControllers() {
        return mUnmodifiableControllerServices;
    }
}
