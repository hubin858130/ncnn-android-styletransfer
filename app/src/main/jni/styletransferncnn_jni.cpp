// Tencent is pleased to support the open source community by making ncnn available.
//
// Copyright (C) 2019 THL A29 Limited, a Tencent company. All rights reserved.
//
// Licensed under the BSD 3-Clause License (the "License"); you may not use this file except
// in compliance with the License. You may obtain a copy of the License at
//
// https://opensource.org/licenses/BSD-3-Clause
//
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

#include <android/asset_manager_jni.h>
#include <android/asset_manager.h>
#include <android/bitmap.h>
#include <android/log.h>

#include <jni.h>

#include <string>
#include <vector>

// ncnn
#include "net.h"

#include "styletransfer.id.h"
#include "styletransfer.param.bin.h"

#include <sys/time.h>
#include <unistd.h>

static struct timeval tv_begin;
static struct timeval tv_end;
static double elasped;

static void bench_start()
{
    gettimeofday(&tv_begin, NULL);
}

static void bench_end(const char* comment)
{
    gettimeofday(&tv_end, NULL);
    elasped = ((tv_end.tv_sec - tv_begin.tv_sec) * 1000000.0f + tv_end.tv_usec - tv_begin.tv_usec) / 1000.0f;
//     fprintf(stderr, "%.2fms   %s\n", elasped, comment);
    __android_log_print(ANDROID_LOG_DEBUG, "StyleTransferNcnn", "%.2fms   %s", elasped, comment);
}

static int load_net_from_asset(ncnn::Net& net, AAssetManager* mgr, const char* model_path)
{
    // load param
    int ret0 = net.load_param(styletransfer_param_bin);

    // load bin
    AAsset* asset = AAssetManager_open(mgr, model_path, AASSET_MODE_STREAMING);

    off_t start;
    off_t length;
    int fd = AAsset_openFileDescriptor(asset, &start, &length);

    FILE* fp = fdopen(fd, "rb");
    fseek(fp, start, SEEK_CUR);

    int ret1 = net.load_model(fp);

    fclose(fp);// it will close fd too

    AAsset_close(asset);

    __android_log_print(ANDROID_LOG_DEBUG, "StyleTransferNcnn", "load_net_from_asset %d %d %d", ret0, ret1, (int)length);

    return 0;
}

static ncnn::UnlockedPoolAllocator g_blob_pool_allocator;
static ncnn::PoolAllocator g_workspace_pool_allocator;

static ncnn::Net styletransfernet[5];

extern "C" {

JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "StyleTransferNcnn", "JNI_OnLoad");

    ncnn::create_gpu_instance();

    return JNI_VERSION_1_4;
}

JNIEXPORT void JNI_OnUnload(JavaVM* vm, void* reserved)
{
    __android_log_print(ANDROID_LOG_DEBUG, "StyleTransferNcnn", "JNI_OnUnload");

    ncnn::destroy_gpu_instance();
}

// public native boolean Init(AssetManager mgr);
JNIEXPORT jboolean JNICALL Java_com_tencent_styletransferncnn_StyleTransferNcnn_Init(JNIEnv* env, jobject thiz, jobject assetManager)
{
    ncnn::Option opt;
    opt.lightmode = true;
    opt.num_threads = 4;
    opt.blob_allocator = &g_blob_pool_allocator;
    opt.workspace_allocator = &g_workspace_pool_allocator;

    // use vulkan compute
    if (ncnn::get_gpu_count() != 0)
        opt.use_vulkan_compute = true;

    AAssetManager* mgr = AAssetManager_fromJava(env, assetManager);

    const char* model_paths[5] = {"candy.bin", "mosaic.bin", "pointilism.bin", "rain_princess.bin", "udnie.bin"};
    for (int i=0; i<5; i++)
    {
        styletransfernet[i].opt = opt;

        if (load_net_from_asset(styletransfernet[i], mgr, model_paths[i]) != 0)
            return JNI_FALSE;
    }

    return JNI_TRUE;
}

// public native Bitmap StyleTransfer(Bitmap bitmap, int style_type, boolean use_gpu);
JNIEXPORT jboolean JNICALL Java_com_tencent_styletransferncnn_StyleTransferNcnn_StyleTransfer(JNIEnv* env, jobject thiz, jobject bitmap, jint style_type, jboolean use_gpu)
{
    if (style_type < 0 || style_type >= 5)
        return JNI_FALSE;

    if (use_gpu == JNI_TRUE && ncnn::get_gpu_count() == 0)
        return JNI_FALSE;

    bench_start();

    AndroidBitmapInfo info;
    AndroidBitmap_getInfo(env, bitmap, &info);
    if (info.format != ANDROID_BITMAP_FORMAT_RGBA_8888)
        return JNI_FALSE;

    int width = info.width;
    int height = info.height;

    void* indata;
    AndroidBitmap_lockPixels(env, bitmap, &indata);

    const int downscale_ratio = 2;

    // ncnn from bitmap
    ncnn::Mat in = ncnn::Mat::from_pixels_resize((const unsigned char*)indata, ncnn::Mat::PIXEL_RGBA2RGB, width, height, width / downscale_ratio, height / downscale_ratio);

    // styletransfer
    ncnn::Mat out;
    {
        ncnn::Extractor ex = styletransfernet[style_type].create_extractor();

        ex.set_vulkan_compute(use_gpu);

        ex.input(styletransfer_param_id::BLOB_input1, in);

        ex.extract(styletransfer_param_id::BLOB_output1, out);
    }

    // ncnn to bitmap
    ncnn::Mat out_rgb(width, height, (size_t)3u, 3);
    out.to_pixels_resize((unsigned char*)out_rgb.data, ncnn::Mat::PIXEL_RGB, width, height);

    // fill
    const unsigned char* p0 = out_rgb;
    unsigned char* p1 = (unsigned char*)indata;
    for (int i=0; i<width*height; i++)
    {
        p1[0] = p0[0];
        p1[1] = p0[1];
        p1[2] = p0[2];
        p1[3] = 255;

        p0 += 3;
        p1 += 4;
    }

    AndroidBitmap_unlockPixels(env, bitmap);

    bench_end("styletransfer");

    return JNI_TRUE;
}

}