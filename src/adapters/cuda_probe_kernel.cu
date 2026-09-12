// Real CUDA execution proof kernel.
//
// This translation unit is compiled by nvcc only when a CUDA toolkit is
// available. The kernel and its host wrapper perform a complete allocate ->
// transfer -> execute -> copy-back -> verify -> release cycle on genuine
// hardware. The verification is performed against an independently computed
// CPU reference, so a successful run proves that device execution actually
// happened and produced the expected result.

#include <cuda_runtime.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

constexpr int kElementCount = 1 << 20;

__global__ void haf_scale_kernel(const float* __restrict__ input, float* __restrict__ output, int count, float scale,
                                 float bias) {
    const int index = blockIdx.x * blockDim.x + threadIdx.x;
    if (index < count) {
        output[index] = input[index] * scale + bias;
    }
}

void write_error(char* buffer, int size, const char* text) {
    if (buffer == nullptr || size <= 0) {
        return;
    }
    const std::size_t length = std::strlen(text);
    const std::size_t limit = static_cast<std::size_t>(size) - 1U;
    const std::size_t copy = length < limit ? length : limit;
    std::memcpy(buffer, text, copy);
    buffer[copy] = '\0';
}

}  // namespace

extern "C" int haf_cuda_probe_run(int device_index, std::uint64_t* bytes_transferred, std::uint64_t* free_before,
                                  std::uint64_t* free_after, char* error_buffer, int error_buffer_size) {
    cudaError_t status = cudaSetDevice(device_index);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 1;
    }

    std::size_t free_memory = 0;
    std::size_t total_memory = 0;
    status = cudaMemGetInfo(&free_memory, &total_memory);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 2;
    }
    if (free_before != nullptr) {
        *free_before = static_cast<std::uint64_t>(free_memory);
    }

    const std::size_t bytes = static_cast<std::size_t>(kElementCount) * sizeof(float);
    float* device_input = nullptr;
    float* device_output = nullptr;
    status = cudaMalloc(reinterpret_cast<void**>(&device_input), bytes);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 3;
    }
    status = cudaMalloc(reinterpret_cast<void**>(&device_output), bytes);
    if (status != cudaSuccess) {
        cudaFree(device_input);
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 4;
    }

    float* host_input = static_cast<float*>(std::malloc(bytes));
    float* host_output = static_cast<float*>(std::malloc(bytes));
    if (host_input == nullptr || host_output == nullptr) {
        std::free(host_input);
        std::free(host_output);
        cudaFree(device_input);
        cudaFree(device_output);
        write_error(error_buffer, error_buffer_size, "host allocation failed");
        return 5;
    }
    for (int i = 0; i < kElementCount; ++i) {
        host_input[i] = static_cast<float>(i % 1024) * 0.5F - 3.0F;
        host_output[i] = -12345.0F;
    }

    const float scale = 2.5F;
    const float bias = -7.25F;

    status = cudaMemcpy(device_input, host_input, bytes, cudaMemcpyHostToDevice);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        std::free(host_input);
        std::free(host_output);
        cudaFree(device_input);
        cudaFree(device_output);
        return 6;
    }

    const int threads = 256;
    const int blocks = (kElementCount + threads - 1) / threads;
    haf_scale_kernel<<<blocks, threads>>>(device_input, device_output, kElementCount, scale, bias);
    status = cudaGetLastError();
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        std::free(host_input);
        std::free(host_output);
        cudaFree(device_input);
        cudaFree(device_output);
        return 7;
    }
    status = cudaDeviceSynchronize();
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        std::free(host_input);
        std::free(host_output);
        cudaFree(device_input);
        cudaFree(device_output);
        return 8;
    }

    status = cudaMemcpy(host_output, device_output, bytes, cudaMemcpyDeviceToHost);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        std::free(host_input);
        std::free(host_output);
        cudaFree(device_input);
        cudaFree(device_output);
        return 9;
    }

    int mismatches = 0;
    for (int i = 0; i < kElementCount; ++i) {
        const float expected = host_input[i] * scale + bias;
        const float observed = host_output[i];
        if (std::fabs(expected - observed) > 1.0e-3F) {
            ++mismatches;
            if (mismatches <= 4) {
                char message[160];
                std::snprintf(message, sizeof(message), "mismatch at %d: expected %.6f observed %.6f", i,
                              static_cast<double>(expected), static_cast<double>(observed));
                write_error(error_buffer, error_buffer_size, message);
            }
        }
    }

    std::free(host_input);
    std::free(host_output);
    cudaFree(device_input);
    cudaFree(device_output);
    status = cudaDeviceSynchronize();
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 10;
    }

    status = cudaMemGetInfo(&free_memory, &total_memory);
    if (status != cudaSuccess) {
        write_error(error_buffer, error_buffer_size, cudaGetErrorString(status));
        return 11;
    }
    if (free_after != nullptr) {
        *free_after = static_cast<std::uint64_t>(free_memory);
    }
    if (bytes_transferred != nullptr) {
        // Two host-to-device / device-to-host transfers plus both allocations
        // were touched; report the bytes actually moved across the bus.
        *bytes_transferred = static_cast<std::uint64_t>(bytes) * 2ULL;
    }
    return mismatches == 0 ? 0 : 12;
}
