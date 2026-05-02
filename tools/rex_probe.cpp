#include "velociloops.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "usage: rex_probe FILE.rx2\n";
        return 2;
    }

    VLError err = VL_OK;
    VLFile file = vl_open(argv[1], &err);
    if (!file) {
        std::cerr << "vl_open failed: " << vl_error_string(err) << "\n";
        return 1;
    }

    VLFileInfo info = {};
    err = vl_get_info(file, &info);
    if (err != VL_OK) {
        std::cerr << "vl_get_info failed: " << vl_error_string(err) << "\n";
        vl_close(file);
        return 1;
    }

    std::cout << "file=" << argv[1] << "\n";
    std::cout << "channels=" << info.channels << " sample_rate=" << info.sample_rate
              << " slices=" << info.slice_count << " frames=" << info.total_frames
              << " tempo_bpm=" << (info.tempo / 1000.0) << "\n";

    const int n = std::min<int>(info.slice_count, 8);
    for (int i = 0; i < n; ++i) {
        VLSliceInfo si = {};
        err = vl_get_slice_info(file, i, &si);
        if (err != VL_OK) {
            std::cerr << "slice info failed at " << i << ": " << vl_error_string(err) << "\n";
            vl_close(file);
            return 1;
        }
        const int frames = vl_get_slice_frame_count(file, i);
        std::vector<float> l(frames), r(frames);
        int32_t written = 0;
        err = vl_decode_slice(file, i, l.data(), r.data(), 0, frames, &written);
        if (err != VL_OK || written != frames) {
            std::cerr << "decode failed at " << i << ": " << vl_error_string(err) << "\n";
            vl_close(file);
            return 1;
        }
        float peak = 0.f;
        for (int j = 0; j < frames; ++j) {
            peak = std::max(peak, std::max(std::fabs(l[j]), std::fabs(r[j])));
        }
        std::cout << "slice " << i << " ppq=" << si.ppq_pos << " start=" << si.sample_start
                  << " raw_len=" << si.sample_length << " rendered=" << frames
                  << " peak=" << peak << "\n";
    }

    vl_close(file);
    return 0;
}
