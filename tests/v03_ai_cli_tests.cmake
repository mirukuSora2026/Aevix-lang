# ========================
#
# Created By : miruku_sora2026
#
# File name : v03_ai_cli_tests.cmake
#
# Description :
#     CPU Shape·Tensor·Quantized의 실제 함수, 정적 차원과 소유권 제약을 AOT·JIT로 확인한다.
#     Check real CPU Shape/Tensor/Quantized functions, static dimensions, and ownership through AOT/JIT.
#
# ========================
if(NOT DEFINED AEVIXC OR NOT DEFINED SOURCE_DIR OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC, SOURCE_DIR, and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)
function(ai_run label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN} RESULT_VARIABLE status
        OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${status}" STREQUAL "${expected}" OR
        (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${status}\n${output}${error}")
    endif()
endfunction()
foreach(name shape tensor quantized)
    set(source "${SOURCE_DIR}/tests/v03_library_${name}.avx")
    ai_run("${name} check" 0 "" --check "${source}")
    ai_run("${name} AOT O0" 0 "" --run -O0 "${source}")
    ai_run("${name} AOT O2" 0 "" --run -O2 "${source}")
    if(version MATCHES "ORC JIT enabled")
        ai_run("${name} JIT O0" 0 "" --jit -O0 "${source}")
        ai_run("${name} JIT O2" 0 "" --jit -O2 "${source}")
    endif()
endforeach()
function(ai_reject name source pattern)
    set(path "${TEST_DIR}/${name}.avx")
    file(WRITE "${path}" "${source}\n")
    ai_run("reject ${name}" 1 "${pattern}" --check "${path}")
endfunction()
ai_reject(no_import "i32 main() { Tensor<f64, Shape2<2,3>> value; return 0; }" "Unknown|import")
ai_reject(static_contraction [=[
import shape;
import tensor;
void wrong(&tensor.Tensor<f64, shape.Shape2<2,3>> left, &tensor.Tensor<f64, shape.Shape2<4,2>> right) {
    tensor.matmul(left, right);
    return;
}
i32 main() { return 0; }
]=] "infer|matching|conflict|consistent")
ai_reject(private_tensor_storage [=[
import tensor;
import shape;
Vector<i32> expose(tensor.Tensor<i32, shape.Shape1<2>> value) { return value.storage; }
i32 main() { return 0; }
]=] "private|Private|accessible")
ai_reject(forged_tensor [=[
import tensor;
import shape;
i32 main() {
    Result<Vector<i32>, CollectionError> values = [];
    tensor.Tensor<i32, shape.Shape1<2>> forged = tensor.Tensor<i32, shape.Shape1<2>> { storage = values, dimensions = shape.Shape1<2>.new(), count = 2 };
    return 0;
}
]=] "private|Private|accessible")
ai_reject(view_escape [=[
import tensor;
import shape;
Slice<i32> escape(tensor.Tensor<i32, shape.Shape1<2>> value) { return value.view(); }
i32 main() { return 0; }
]=] "local|outlive|borrow|reference|loan")
ai_reject(view_conflict [=[
import tensor;
import shape;
i32 wrong(mut tensor.Tensor<i32, shape.Shape1<2>> value) {
    Slice<i32> shared = value.view();
    value.fill(0);
    return shared[0];
}
i32 main() { return 0; }
]=] "borrow|loan|exclusive")
ai_reject(descriptor_escape [=[
import tensor;
import shape;
Result<tensor.TensorView<i32, shape.Shape1<2>>, tensor.TensorError> escape(tensor.Tensor<i32, shape.Shape1<2>> value) {
    return value.reshape_view(shape.Shape1<2>.new());
}
i32 main() { return 0; }
]=] "local|outlive|borrow|reference|loan")
ai_reject(descriptor_conflict [=[
import tensor;
import shape;
Result<i32, tensor.TensorError> wrong(mut tensor.Tensor<i32, shape.Shape1<2>> value) {
    tensor.TensorView<i32, shape.Shape1<2>> shared = value.reshape_view(shape.Shape1<2>.new())?;
    value.fill(0)?;
    return Result.ok(shared.view()[0]);
}
i32 main() { return 0; }
]=] "borrow|loan|exclusive")
ai_reject(slice_descriptor_escape [=[
import tensor;
import shape;
Result<tensor.TensorView<i32, shape.Shape1<2>>, tensor.TensorError> escape() {
    i32 values[2] = [1, 2];
    return tensor.TensorView<i32, shape.Shape1<2>>.from_slice(Slice.from_array(&values), shape.Shape1<2>.new());
}
i32 main() { return 0; }
]=] "local|outlive|borrow|reference|loan")
ai_reject(moved_tensor [=[
import tensor;
import shape;
usize wrong(tensor.Tensor<i32, shape.Shape1<2>> value) { value.into_vector(); return value.element_count(); }
i32 main() { return 0; }
]=] "move|unavailable|ownership")
ai_reject(dynamic_shape_moves [=[
import shape;
usize wrong(shape.DynamicShape<2> value) { shape.DynamicShape<2> moved = value; return value.rank(); }
i32 main() { return 0; }
]=] "move|unavailable|ownership")
ai_reject(private_affine [=[
import quantized;
i32 main() { quantized.Affine<i8> value = quantized.Affine<i8> { scale_value = -1.0, zero_value = 0i8 }; return 0; }
]=] "private|Private|accessible")
ai_reject(noninteger_storage [=[
import quantized;
i32 main() { quantized.Affine<bool>.new(1.0, false); return 0; }
]=] "Numeric conversion.*incompatible.*domains")
ai_reject(per_axis_view_escape [=[
import quantized;
Slice<f64> escape(quantized.PerAxis<i8> value) { return value.scales(); }
i32 main() { return 0; }
]=] "local|outlive|borrow|reference|loan")
message(STATUS "CPU Shape, Tensor, and Quantized source foundations and rejection contracts passed")
