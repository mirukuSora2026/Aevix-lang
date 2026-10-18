# ========================
#
# Created By : miruku_sora2026
#
# File name : library_error_tests.cmake
#
# Description :
#     실제 오류 라이브러리의 분류·동적 소유권·뷰 수명과 잘못된 변환을 검사한다.
#     Check actual library error categories, dynamic ownership, view lifetimes, and invalid conversions.
#
# ========================

if(NOT DEFINED AEVIXC OR NOT DEFINED TEST_DIR)
    message(FATAL_ERROR "AEVIXC and TEST_DIR are required")
endif()
file(MAKE_DIRECTORY "${TEST_DIR}")
execute_process(COMMAND "${AEVIXC}" --version OUTPUT_VARIABLE version)

function(error_command label expected pattern)
    execute_process(COMMAND "${AEVIXC}" ${ARGN}
        RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 90)
    if(NOT "${result}" STREQUAL "${expected}" OR
       (NOT "${pattern}" STREQUAL "" AND NOT "${output}${error}" MATCHES "${pattern}"))
        message(FATAL_ERROR "${label}: expected ${expected}/${pattern}, got ${result}: ${output}${error}")
    endif()
endfunction()

function(error_program name source)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    error_command("${name} check" 0 "ownership checked" --check "${TEST_DIR}/${name}.avx")
    error_command("${name} AOT" 0 "" --run "${TEST_DIR}/${name}.avx")
    error_command("${name} AOT O2" 0 "" --run -O2 "${TEST_DIR}/${name}.avx")
    if(version MATCHES "ORC JIT enabled")
        error_command("${name} JIT" 0 "" --jit "${TEST_DIR}/${name}.avx")
        error_command("${name} JIT O2" 0 "" --jit -O2 "${TEST_DIR}/${name}.avx")
    endif()
endfunction()

function(reject_error name source pattern)
    file(WRITE "${TEST_DIR}/${name}.avx" "${source}\n")
    error_command("reject ${name}" 1 "${pattern}" --check "${TEST_DIR}/${name}.avx")
endfunction()

error_program(core_adapter [=[
import error { CoreError, AnyError, Error };
import memory { RefCountError };
i32 main() {
    CoreError value_1 = CoreError.Collection { error = CollectionError.AllocationFailure };
    if (!string.equals(value_1.code(), "collection.allocation_failure")) return 1;
    if (!string.equals(value_1.message(), "Memory for the collection could not be allocated.")) return 1;
    CoreError value_2 = CoreError.Collection { error = CollectionError.SizeOverflow };
    if (!string.equals(value_2.code(), "collection.size_overflow")) return 2;
    if (!string.equals(value_2.message(), "The requested collection size is not representable.")) return 2;
    CoreError value_3 = CoreError.Range { error = RangeError.InvalidStep };
    if (!string.equals(value_3.code(), "range.invalid_step")) return 3;
    if (!string.equals(value_3.message(), "The range step is invalid for its direction.")) return 3;
    CoreError value_4 = CoreError.Range { error = RangeError.CountOverflow };
    if (!string.equals(value_4.code(), "range.count_overflow")) return 4;
    if (!string.equals(value_4.message(), "The range element count is not representable.")) return 4;
    CoreError value_5 = CoreError.Bounds { error = BoundsError.OutOfBounds };
    if (!string.equals(value_5.code(), "bounds.out_of_bounds")) return 5;
    if (!string.equals(value_5.message(), "The requested index is outside the valid bounds.")) return 5;
    CoreError value_6 = CoreError.Bounds { error = BoundsError.ReversedRange };
    if (!string.equals(value_6.code(), "bounds.reversed_range")) return 6;
    if (!string.equals(value_6.message(), "The range start is greater than its end.")) return 6;
    CoreError value_7 = CoreError.Text { error = TextError.AllocationFailure };
    if (!string.equals(value_7.code(), "text.allocation_failure")) return 7;
    if (!string.equals(value_7.message(), "Memory for the text could not be allocated.")) return 7;
    CoreError value_8 = CoreError.Text { error = TextError.SizeOverflow };
    if (!string.equals(value_8.code(), "text.size_overflow")) return 8;
    if (!string.equals(value_8.message(), "The requested text size is not representable.")) return 8;
    CoreError value_9 = CoreError.Text { error = TextError.InvalidUtf8 };
    if (!string.equals(value_9.code(), "text.invalid_utf8")) return 9;
    if (!string.equals(value_9.message(), "The input is not valid UTF-8.")) return 9;
    CoreError value_10 = CoreError.Text { error = TextError.InvalidBoundary };
    if (!string.equals(value_10.code(), "text.invalid_boundary")) return 10;
    if (!string.equals(value_10.message(), "The byte offset is not a UTF-8 character boundary.")) return 10;
    CoreError value_11 = CoreError.Parse { error = ParseError.EmptyInput };
    if (!string.equals(value_11.code(), "parse.empty_input")) return 11;
    if (!string.equals(value_11.message(), "Input contains no scalar token.")) return 11;
    CoreError value_12 = CoreError.Parse { error = ParseError.InvalidSyntax };
    if (!string.equals(value_12.code(), "parse.invalid_syntax")) return 12;
    if (!string.equals(value_12.message(), "Input is not a complete valid scalar representation.")) return 12;
    CoreError value_13 = CoreError.Parse { error = ParseError.OutOfRange };
    if (!string.equals(value_13.code(), "parse.out_of_range")) return 13;
    if (!string.equals(value_13.message(), "Input is outside the requested scalar type range.")) return 13;
    CoreError value_14 = CoreError.Parse { error = ParseError.NonFinite };
    if (!string.equals(value_14.code(), "parse.non_finite")) return 14;
    if (!string.equals(value_14.message(), "A finite scalar value is required.")) return 14;
    CoreError value_15 = CoreError.Parse { error = ParseError.InvalidScalar };
    if (!string.equals(value_15.code(), "parse.invalid_scalar")) return 15;
    if (!string.equals(value_15.message(), "Input is not a valid Unicode scalar.")) return 15;
    CoreError value_16 = CoreError.Integer { error = IntegerError.OutOfRange };
    if (!string.equals(value_16.code(), "integer.out_of_range")) return 16;
    if (!string.equals(value_16.message(), "The integer result is outside the requested type range.")) return 16;
    CoreError value_17 = CoreError.Float { error = FloatError.OutOfRange };
    if (!string.equals(value_17.code(), "float.out_of_range")) return 17;
    if (!string.equals(value_17.message(), "The floating-point result is outside the requested type range.")) return 17;
    CoreError value_18 = CoreError.Float { error = FloatError.NonFinite };
    if (!string.equals(value_18.code(), "float.non_finite")) return 18;
    if (!string.equals(value_18.message(), "A finite floating-point value is required.")) return 18;
    CoreError value_19 = CoreError.Float { error = FloatError.NonExact };
    if (!string.equals(value_19.code(), "float.non_exact")) return 19;
    if (!string.equals(value_19.message(), "The conversion cannot preserve the value exactly.")) return 19;
    CoreError value_20 = CoreError.RefCount { error = RefCountError.CountOverflow };
    if (!string.equals(value_20.code(), "memory.ref_count_overflow")) return 20;
    if (!string.equals(value_20.message(), "The shared or weak reference count cannot be increased.")) return 20;
    Result<char, CharError> char_result = Char.from_u32(55296u32);
    switch (move char_result) {
        case Result<char, CharError>.Err { CharError char_failure = error }:
            CoreError char_adapter = CoreError.Char { error = char_failure };
            &dyn Error char_view = &char_adapter;
            if (!string.equals(char_view.code(), "char.invalid_scalar")) return 21;
            if (!string.equals(char_view.message(), "The value is not a valid Unicode scalar.")) return 22;
            switch (char_adapter) {
                case CoreError.Char { &CharError char_original = error }:
                    switch (*char_original) {
                        case CharError.InvalidScalar { u32 char_scalar = value }:
                            if (char_scalar != 55296u32) return 23;
                            break;
                    }
                    break;
                default: return 24; break;
            }
            Result<AnyError, InputError<CoreError>> char_packed = AnyError.pack(char_adapter);
            switch (move char_packed) {
                case Result<AnyError, InputError<CoreError>>.Ok { AnyError char_erased = value }:
                    if (!string.equals(char_erased.code(), "char.invalid_scalar")) return 25;
                    break;
                case Result<AnyError, InputError<CoreError>>.Err: return 26; break;
            }
            break;
        case Result<char, CharError>.Ok: return 27; break;
    }
    CoreError original = CoreError.Parse { error = ParseError.InvalidSyntax };
    Result<AnyError, InputError<CoreError>> packed = AnyError.pack(original);
    switch (move packed) {
        case Result<AnyError, InputError<CoreError>>.Ok { AnyError erased = value }:
            if (!string.equals(erased.code(), "parse.invalid_syntax")) return 50;
            break;
        case Result<AnyError, InputError<CoreError>>.Err: return 51; break;
    }
    return 0;
}
]=])

error_program(all_variants [=[
import error { Error, IoError, ReadError, AnyError, FormatError, FileError, PathError, ProcessError, EnvError, TimeError, SyncError, TaskError, ShapeError, TensorError, QuantizationError, MapError };
i32 main() {
    ProcessError termination = ProcessError.TerminationFailed { os_code = 13 };
    if (!string.equals(termination.code(), "process.termination_failed") || string.byte_len(termination.message()) == 0) return 1;
    IoError value_1 = IoError.ReadFailed { os_code = 13, progress = 7 };
    if (string.byte_len(value_1.code()) == 0 || string.byte_len(value_1.message()) == 0) return 1;
    IoError value_2 = IoError.WriteFailed { os_code = 13, progress = 7 };
    if (string.byte_len(value_2.code()) == 0 || string.byte_len(value_2.message()) == 0) return 1;
    IoError value_3 = IoError.FlushFailed { os_code = 13 };
    if (string.byte_len(value_3.code()) == 0 || string.byte_len(value_3.message()) == 0) return 1;
    IoError value_4 = IoError.Interrupted;
    if (string.byte_len(value_4.code()) == 0 || string.byte_len(value_4.message()) == 0) return 1;
    IoError value_5 = IoError.WouldBlock;
    if (string.byte_len(value_5.code()) == 0 || string.byte_len(value_5.message()) == 0) return 1;
    IoError value_6 = IoError.Closed;
    if (string.byte_len(value_6.code()) == 0 || string.byte_len(value_6.message()) == 0) return 1;
    IoError value_7 = IoError.InvalidUtf8;
    if (string.byte_len(value_7.code()) == 0 || string.byte_len(value_7.message()) == 0) return 1;
    IoError value_8 = IoError.InputTooLong;
    if (string.byte_len(value_8.code()) == 0 || string.byte_len(value_8.message()) == 0) return 1;
    IoError value_9 = IoError.AllocationFailure;
    if (string.byte_len(value_9.code()) == 0 || string.byte_len(value_9.message()) == 0) return 1;
    IoError value_10 = IoError.SizeOverflow;
    if (string.byte_len(value_10.code()) == 0 || string.byte_len(value_10.message()) == 0) return 1;
    IoError value_11 = IoError.Unsupported;
    if (string.byte_len(value_11.code()) == 0 || string.byte_len(value_11.message()) == 0) return 1;
    FormatError value_12 = FormatError.InvalidFormat;
    if (string.byte_len(value_12.code()) == 0 || string.byte_len(value_12.message()) == 0) return 1;
    FormatError value_13 = FormatError.MissingArgument;
    if (string.byte_len(value_13.code()) == 0 || string.byte_len(value_13.message()) == 0) return 1;
    FormatError value_14 = FormatError.UnexpectedArgument;
    if (string.byte_len(value_14.code()) == 0 || string.byte_len(value_14.message()) == 0) return 1;
    FormatError value_15 = FormatError.TypeMismatch;
    if (string.byte_len(value_15.code()) == 0 || string.byte_len(value_15.message()) == 0) return 1;
    FormatError value_16 = FormatError.InvalidWidth;
    if (string.byte_len(value_16.code()) == 0 || string.byte_len(value_16.message()) == 0) return 1;
    FormatError value_17 = FormatError.InvalidPrecision;
    if (string.byte_len(value_17.code()) == 0 || string.byte_len(value_17.message()) == 0) return 1;
    FormatError value_18 = FormatError.AllocationFailure;
    if (string.byte_len(value_18.code()) == 0 || string.byte_len(value_18.message()) == 0) return 1;
    FormatError value_19 = FormatError.SizeOverflow;
    if (string.byte_len(value_19.code()) == 0 || string.byte_len(value_19.message()) == 0) return 1;
    FileError value_20 = FileError.NotFound { os_code = 13 };
    if (string.byte_len(value_20.code()) == 0 || string.byte_len(value_20.message()) == 0) return 1;
    FileError value_21 = FileError.AlreadyExists { os_code = 13 };
    if (string.byte_len(value_21.code()) == 0 || string.byte_len(value_21.message()) == 0) return 1;
    FileError value_22 = FileError.PermissionDenied { os_code = 13 };
    if (string.byte_len(value_22.code()) == 0 || string.byte_len(value_22.message()) == 0) return 1;
    FileError value_23 = FileError.InvalidHandle;
    if (string.byte_len(value_23.code()) == 0 || string.byte_len(value_23.message()) == 0) return 1;
    FileError value_24 = FileError.NotRegularFile;
    if (string.byte_len(value_24.code()) == 0 || string.byte_len(value_24.message()) == 0) return 1;
    FileError value_25 = FileError.Unsupported;
    if (string.byte_len(value_25.code()) == 0 || string.byte_len(value_25.message()) == 0) return 1;
    FileError value_26 = FileError.Io { error = IoError.Closed };
    if (string.byte_len(value_26.code()) == 0 || string.byte_len(value_26.message()) == 0) return 1;
    PathError value_27 = PathError.InvalidEncoding;
    if (string.byte_len(value_27.code()) == 0 || string.byte_len(value_27.message()) == 0) return 1;
    PathError value_28 = PathError.EmbeddedNull;
    if (string.byte_len(value_28.code()) == 0 || string.byte_len(value_28.message()) == 0) return 1;
    PathError value_29 = PathError.InvalidComponent;
    if (string.byte_len(value_29.code()) == 0 || string.byte_len(value_29.message()) == 0) return 1;
    PathError value_30 = PathError.NotAbsolute;
    if (string.byte_len(value_30.code()) == 0 || string.byte_len(value_30.message()) == 0) return 1;
    PathError value_31 = PathError.AllocationFailure;
    if (string.byte_len(value_31.code()) == 0 || string.byte_len(value_31.message()) == 0) return 1;
    PathError value_32 = PathError.SizeOverflow;
    if (string.byte_len(value_32.code()) == 0 || string.byte_len(value_32.message()) == 0) return 1;
    PathError value_33 = PathError.Os { os_code = 13 };
    if (string.byte_len(value_33.code()) == 0 || string.byte_len(value_33.message()) == 0) return 1;
    ProcessError value_34 = ProcessError.SpawnFailed { os_code = 13 };
    if (string.byte_len(value_34.code()) == 0 || string.byte_len(value_34.message()) == 0) return 1;
    ProcessError value_35 = ProcessError.WaitFailed { os_code = 13 };
    if (string.byte_len(value_35.code()) == 0 || string.byte_len(value_35.message()) == 0) return 1;
    ProcessError value_36 = ProcessError.InvalidArgument;
    if (string.byte_len(value_36.code()) == 0 || string.byte_len(value_36.message()) == 0) return 1;
    ProcessError value_37 = ProcessError.AlreadyReaped;
    if (string.byte_len(value_37.code()) == 0 || string.byte_len(value_37.message()) == 0) return 1;
    ProcessError value_38 = ProcessError.TimedOut;
    if (string.byte_len(value_38.code()) == 0 || string.byte_len(value_38.message()) == 0) return 1;
    ProcessError value_39 = ProcessError.AllocationFailure;
    if (string.byte_len(value_39.code()) == 0 || string.byte_len(value_39.message()) == 0) return 1;
    ProcessError value_40 = ProcessError.Unsupported;
    if (string.byte_len(value_40.code()) == 0 || string.byte_len(value_40.message()) == 0) return 1;
    EnvError value_41 = EnvError.NotFound;
    if (string.byte_len(value_41.code()) == 0 || string.byte_len(value_41.message()) == 0) return 1;
    EnvError value_42 = EnvError.InvalidName;
    if (string.byte_len(value_42.code()) == 0 || string.byte_len(value_42.message()) == 0) return 1;
    EnvError value_43 = EnvError.InvalidEncoding;
    if (string.byte_len(value_43.code()) == 0 || string.byte_len(value_43.message()) == 0) return 1;
    EnvError value_44 = EnvError.EmbeddedNull;
    if (string.byte_len(value_44.code()) == 0 || string.byte_len(value_44.message()) == 0) return 1;
    EnvError value_45 = EnvError.AllocationFailure;
    if (string.byte_len(value_45.code()) == 0 || string.byte_len(value_45.message()) == 0) return 1;
    EnvError value_46 = EnvError.Os { os_code = 13 };
    if (string.byte_len(value_46.code()) == 0 || string.byte_len(value_46.message()) == 0) return 1;
    TimeError value_47 = TimeError.OutOfRange;
    if (string.byte_len(value_47.code()) == 0 || string.byte_len(value_47.message()) == 0) return 1;
    TimeError value_48 = TimeError.InvalidDuration;
    if (string.byte_len(value_48.code()) == 0 || string.byte_len(value_48.message()) == 0) return 1;
    TimeError value_49 = TimeError.ClockUnavailable;
    if (string.byte_len(value_49.code()) == 0 || string.byte_len(value_49.message()) == 0) return 1;
    TimeError value_50 = TimeError.Interrupted;
    if (string.byte_len(value_50.code()) == 0 || string.byte_len(value_50.message()) == 0) return 1;
    TimeError value_51 = TimeError.Os { os_code = 13 };
    if (string.byte_len(value_51.code()) == 0 || string.byte_len(value_51.message()) == 0) return 1;
    SyncError value_52 = SyncError.Poisoned;
    if (string.byte_len(value_52.code()) == 0 || string.byte_len(value_52.message()) == 0) return 1;
    SyncError value_53 = SyncError.WouldBlock;
    if (string.byte_len(value_53.code()) == 0 || string.byte_len(value_53.message()) == 0) return 1;
    SyncError value_54 = SyncError.TimedOut;
    if (string.byte_len(value_54.code()) == 0 || string.byte_len(value_54.message()) == 0) return 1;
    SyncError value_55 = SyncError.Closed;
    if (string.byte_len(value_55.code()) == 0 || string.byte_len(value_55.message()) == 0) return 1;
    SyncError value_56 = SyncError.ResourceExhausted;
    if (string.byte_len(value_56.code()) == 0 || string.byte_len(value_56.message()) == 0) return 1;
    SyncError value_57 = SyncError.Unsupported;
    if (string.byte_len(value_57.code()) == 0 || string.byte_len(value_57.message()) == 0) return 1;
    SyncError value_58 = SyncError.Os { os_code = 13 };
    if (string.byte_len(value_58.code()) == 0 || string.byte_len(value_58.message()) == 0) return 1;
    TaskError value_59 = TaskError.SpawnFailed { os_code = 13 };
    if (string.byte_len(value_59.code()) == 0 || string.byte_len(value_59.message()) == 0) return 1;
    TaskError value_60 = TaskError.Cancelled;
    if (string.byte_len(value_60.code()) == 0 || string.byte_len(value_60.message()) == 0) return 1;
    TaskError value_61 = TaskError.TimedOut;
    if (string.byte_len(value_61.code()) == 0 || string.byte_len(value_61.message()) == 0) return 1;
    TaskError value_62 = TaskError.AlreadyJoined;
    if (string.byte_len(value_62.code()) == 0 || string.byte_len(value_62.message()) == 0) return 1;
    TaskError value_63 = TaskError.ScopeClosed;
    if (string.byte_len(value_63.code()) == 0 || string.byte_len(value_63.message()) == 0) return 1;
    TaskError value_64 = TaskError.AllocationFailure;
    if (string.byte_len(value_64.code()) == 0 || string.byte_len(value_64.message()) == 0) return 1;
    TaskError value_65 = TaskError.Unsupported;
    if (string.byte_len(value_65.code()) == 0 || string.byte_len(value_65.message()) == 0) return 1;
    ShapeError value_66 = ShapeError.RankMismatch;
    if (string.byte_len(value_66.code()) == 0 || string.byte_len(value_66.message()) == 0) return 1;
    ShapeError value_67 = ShapeError.ExtentMismatch { axis = 7 };
    if (string.byte_len(value_67.code()) == 0 || string.byte_len(value_67.message()) == 0) return 1;
    ShapeError value_68 = ShapeError.InvalidAxis { axis = 7 };
    if (string.byte_len(value_68.code()) == 0 || string.byte_len(value_68.message()) == 0) return 1;
    ShapeError value_69 = ShapeError.ElementCountOverflow;
    if (string.byte_len(value_69.code()) == 0 || string.byte_len(value_69.message()) == 0) return 1;
    ShapeError value_70 = ShapeError.NotBroadcastable;
    if (string.byte_len(value_70.code()) == 0 || string.byte_len(value_70.message()) == 0) return 1;
    ShapeError value_71 = ShapeError.AllocationFailure;
    if (string.byte_len(value_71.code()) == 0 || string.byte_len(value_71.message()) == 0) return 1;
    TensorError value_72 = TensorError.Shape { error = ShapeError.RankMismatch };
    if (string.byte_len(value_72.code()) == 0 || string.byte_len(value_72.message()) == 0) return 1;
    TensorError value_73 = TensorError.OutOfBounds;
    if (string.byte_len(value_73.code()) == 0 || string.byte_len(value_73.message()) == 0) return 1;
    TensorError value_74 = TensorError.InvalidLayout;
    if (string.byte_len(value_74.code()) == 0 || string.byte_len(value_74.message()) == 0) return 1;
    TensorError value_75 = TensorError.NonContiguous;
    if (string.byte_len(value_75.code()) == 0 || string.byte_len(value_75.message()) == 0) return 1;
    TensorError value_76 = TensorError.Aliasing;
    if (string.byte_len(value_76.code()) == 0 || string.byte_len(value_76.message()) == 0) return 1;
    TensorError value_77 = TensorError.AllocationFailure;
    if (string.byte_len(value_77.code()) == 0 || string.byte_len(value_77.message()) == 0) return 1;
    TensorError value_78 = TensorError.SizeOverflow;
    if (string.byte_len(value_78.code()) == 0 || string.byte_len(value_78.message()) == 0) return 1;
    TensorError value_79 = TensorError.ArithmeticFailure;
    if (string.byte_len(value_79.code()) == 0 || string.byte_len(value_79.message()) == 0) return 1;
    TensorError value_80 = TensorError.Unsupported;
    if (string.byte_len(value_80.code()) == 0 || string.byte_len(value_80.message()) == 0) return 1;
    QuantizationError value_81 = QuantizationError.InvalidScale;
    if (string.byte_len(value_81.code()) == 0 || string.byte_len(value_81.message()) == 0) return 1;
    QuantizationError value_82 = QuantizationError.InvalidZeroPoint;
    if (string.byte_len(value_82.code()) == 0 || string.byte_len(value_82.message()) == 0) return 1;
    QuantizationError value_83 = QuantizationError.InvalidAxis { axis = 7 };
    if (string.byte_len(value_83.code()) == 0 || string.byte_len(value_83.message()) == 0) return 1;
    QuantizationError value_84 = QuantizationError.MetadataMismatch;
    if (string.byte_len(value_84.code()) == 0 || string.byte_len(value_84.message()) == 0) return 1;
    QuantizationError value_85 = QuantizationError.NonFinite;
    if (string.byte_len(value_85.code()) == 0 || string.byte_len(value_85.message()) == 0) return 1;
    QuantizationError value_86 = QuantizationError.OutOfRange;
    if (string.byte_len(value_86.code()) == 0 || string.byte_len(value_86.message()) == 0) return 1;
    QuantizationError value_87 = QuantizationError.Shape { error = ShapeError.RankMismatch };
    if (string.byte_len(value_87.code()) == 0 || string.byte_len(value_87.message()) == 0) return 1;
    QuantizationError value_88 = QuantizationError.AllocationFailure;
    if (string.byte_len(value_88.code()) == 0 || string.byte_len(value_88.message()) == 0) return 1;
    QuantizationError value_89 = QuantizationError.Unsupported;
    if (string.byte_len(value_89.code()) == 0 || string.byte_len(value_89.message()) == 0) return 1;
    MapError value_90 = MapError.AllocationFailure;
    if (string.byte_len(value_90.code()) == 0 || string.byte_len(value_90.message()) == 0) return 1;
    MapError value_91 = MapError.SizeOverflow;
    if (string.byte_len(value_91.code()) == 0 || string.byte_len(value_91.message()) == 0) return 1;
    MapError value_92 = MapError.MissingKey;
    if (string.byte_len(value_92.code()) == 0 || string.byte_len(value_92.message()) == 0) return 1;
    MapError value_93 = MapError.DuplicateKey;
    if (string.byte_len(value_93.code()) == 0 || string.byte_len(value_93.message()) == 0) return 1;
    MapError value_94 = MapError.CapacityExceeded;
    if (string.byte_len(value_94.code()) == 0 || string.byte_len(value_94.message()) == 0) return 1;
    ReadError parsed = ReadError.Parse { error = ParseError.InvalidSyntax };
    ReadError duplicate = parsed;
    if (!string.equals(parsed.code(), "parse.invalid_syntax") || !string.equals(duplicate.message(), "Input is not a complete valid scalar representation.")) return 2;
    IoError io = IoError.WriteFailed { os_code = 32, progress = 9 };
    ReadError wrapped = ReadError.Io { error = io };
    if (!string.equals(wrapped.code(), "io.write_failed")) return 3;
    switch (io) {
        case IoError.WriteFailed { i32 native = os_code, usize transferred = progress }:
            if (native != 32 || transferred != 9) return 4;
            break;
        default: return 5; break;
    }
    return 0;
}
]=])

error_program(owned [=[
import memory { Box };
import error { Error, IoError, ReadError, AnyError };
i32 main() {
    IoError original = IoError.WriteFailed { os_code = 32, progress = 9 };
    IoError copied = original;
    if (!string.equals(original.code(), "io.write_failed")) return 1;
    switch (copied) {
        case IoError.WriteFailed { i32 native = os_code, usize progress = progress }:
            if (native != 32 || progress != 9) return 2;
            break;
        default: return 3; break;
    }
    ReadError parse = ReadError.Parse { error = ParseError.OutOfRange };
    if (!string.equals(parse.code(), "parse.out_of_range")) return 4;
    if (!string.equals(parse.message(), "Input is outside the requested scalar type range.")) return 5;
    IoError input = IoError.Closed;
    Result<Box<IoError>, InputError<IoError>> result = Box.new(input);
    switch (move result) {
        case Result<Box<IoError>, InputError<IoError>>.Ok { Box<IoError> concrete = value }:
            Box<dyn Error> boxed = Box.into_dyn(concrete);
            AnyError erased = AnyError.from_box(boxed);
            &dyn Error borrowed = erased.as_error();
            if (!string.equals(borrowed.code(), "io.closed")) return 6;
            if (!string.equals(erased.message(), "The stream or resource is closed.")) return 7;
            break;
        case Result<Box<IoError>, InputError<IoError>>.Err: return 8; break;
    }
    return 0;
}
]=])

error_program(packed [=[
import error { Error, IoError, AnyError };
i32 main() {
    IoError input = IoError.Closed;
    Result<AnyError, InputError<IoError>> result = AnyError.pack(input);
    switch (move result) {
        case Result<AnyError, InputError<IoError>>.Ok { AnyError erased = value }:
            return string.equals(erased.code(), "io.closed") && string.equals(erased.message(), "The stream or resource is closed.") ? 0 : 1;
            break;
        case Result<AnyError, InputError<IoError>>.Err { InputError<IoError> allocation = error }:
            switch (move allocation) {
                case InputError<IoError>.AllocationFailure { IoError original = input }:
                    return string.equals(original.code(), "io.closed") ? 2 : 3;
                    break;
                case InputError<IoError>.SizeOverflow { IoError original = input }:
                    return string.equals(original.code(), "io.closed") ? 4 : 5;
                    break;
                case InputError<IoError>.OutOfBounds: Never.unreachable(); break;
            }
            break;
    }
}
]=])

error_program(owned_text [=[
import error { Error, AnyError };
struct DetailFailure implements Error {
    string detail;
    public DetailFailure new(string value) { return DetailFailure { detail = value }; }
    public string_view code(&DetailFailure self) { return "custom.detail"; }
    public string_view message(&DetailFailure self) { return string.view(&self.detail); }
};
Result<i32, TextError> exercise() {
    string text = string.from_view("owned error payload")?;
    DetailFailure failure = DetailFailure.new(text);
    Result<AnyError, InputError<DetailFailure>> packed = AnyError.pack(failure);
    switch (move packed) {
        case Result<AnyError, InputError<DetailFailure>>.Ok { AnyError owner = value }:
            &dyn Error view = owner.as_error();
            if (!string.equals(owner.code(), "custom.detail")) return Result.ok(1);
            if (!string.equals(view.message(), "owned error payload")) return Result.ok(2);
            return Result.ok(0);
            break;
        case Result<AnyError, InputError<DetailFailure>>.Err { InputError<DetailFailure> rejected = error }:
            switch (move rejected) {
                case InputError<DetailFailure>.AllocationFailure { DetailFailure original = input }:
                    return Result.ok(string.equals(original.message(), "owned error payload") ? 3 : 4);
                    break;
                case InputError<DetailFailure>.SizeOverflow { DetailFailure original = input }:
                    return Result.ok(string.equals(original.message(), "owned error payload") ? 5 : 6);
                    break;
                case InputError<DetailFailure>.OutOfBounds: Never.unreachable(); break;
            }
            break;
    }
}
i32 main() {
    Result<i32, TextError> result = exercise();
    switch (result) {
        case Result<i32, TextError>.Ok { i32 status = value }: return status; break;
        case Result<i32, TextError>.Err: return 7; break;
    }
}
]=])

reject_error(escaping_message [=[
import error { Error, AnyError };
struct TextFailure implements Error {
    string detail;
    public string_view code(&TextFailure self) { return "custom.text"; }
    public string_view message(&TextFailure self) { return string.view(&self.detail); }
};
Result<string_view, InputError<TextFailure>> escape(TextFailure input) {
    AnyError erased = AnyError.pack(input)?;
    return Result.ok(erased.message());
}
i32 main() { return 0; }
]=] "local value")

reject_error(escaping_view [=[
import error { Error, AnyError };
struct StaticFailure implements Error {
    public string_view code(&StaticFailure self) { return "custom.static"; }
    public string_view message(&StaticFailure self) { return "static failure"; }
};
Result<&dyn Error, InputError<StaticFailure>> escape(StaticFailure input) {
    AnyError erased = AnyError.pack(input)?;
    return Result.ok(erased.as_error());
}
i32 main() { return 0; }
]=] "local value")

reject_error(moved_owner [=[
import error { Error, AnyError };
i32 inspect(AnyError input) {
    AnyError moved = input;
    return string.byte_len(input.code()) == 0 ? 0 : 1;
}
i32 main() { return 0; }
]=] "moved or unavailable")

reject_error(private_owner [=[
import memory { Box };
import error { Error, AnyError };
Box<dyn Error> bypass(AnyError input) { return input.owner; }
i32 main() { return 0; }
]=] "private member")

reject_error(borrow_then_move [=[
import error { Error, AnyError };
i32 inspect(AnyError input) {
    &dyn Error view = input.as_error();
    AnyError moved = input;
    return string.byte_len(view.code()) == 0 ? 0 : 1;
}
i32 main() { return 0; }
]=] "while it is borrowed")

reject_error(no_implicit_union [=[
import error { IoError, AnyError };
Result<i32, AnyError> invalid(IoError input) { return Result.err(input); }
i32 main() { return 0; }
]=] "Enum payload requires AnyError.*received IoError")

reject_error(erased_owner_borrow_escape [=[
import error { Error, AnyError };
struct BorrowedFailure implements Error {
    public string_view detail;
    public string_view code(&BorrowedFailure self) { return "borrowed"; }
    public string_view message(&BorrowedFailure self) { return self.detail; }
};
Result<AnyError, TextError> escape() {
    string owner = string.from_view("local owner")?;
    BorrowedFailure failure = BorrowedFailure { detail = string.view(&owner) };
    Result<AnyError, InputError<BorrowedFailure>> result = AnyError.pack(failure);
    switch (move result) {
        case Result<AnyError, InputError<BorrowedFailure>>.Ok { AnyError erased = value }: return Result.ok(erased); break;
        case Result<AnyError, InputError<BorrowedFailure>>.Err: return Result.err(TextError.AllocationFailure); break;
    }
}
i32 main() { return 0; }
]=] "Cannot return a reference to a local value")

message(STATUS "Error library variants, CoreError adapters, erasure, and ownership/type rejections passed")
