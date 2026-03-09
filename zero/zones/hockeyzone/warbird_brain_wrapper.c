// warbird_brain_wrapper.c
// This wrapper includes the onnx2c-generated code and provides the warbird_brain() function
//
// USAGE:
// 1. Copy your marvin_brain_Warbird.c file to this directory
// 2. Rename it to warbird_brain_raw.c (or keep as is and update the include below)
// 3. This wrapper will provide the warbird_brain() function

// Include the onnx2c-generated code
// The generated code has: void entry(const float inputs[1][7], float outputs[1][2])
#include "warbird_brain.c"

// Wrapper function with cleaner signature
void warbird_brain(const float inputs[7], float outputs[2]) {
    // onnx2c expects [1][7] and [1][2] shaped arrays
    // We can cast our 1D arrays to 2D (they have same memory layout)
    entry((const float(*)[7])inputs, (float(*)[2])outputs);
}
