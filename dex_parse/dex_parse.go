package main

import "github.com/golang/exp/mmap"
import "fmt"
import "os"
import "strconv"
import "strings"
import "unsafe"

type header_item struct {
	magic [8]uint8
	checksum uint32
	signature [20]uint8
	file_size uint32
	header_size uint32
	endian_tag uint32
	link_size uint32
	link_off uint32
	map_off uint32
	string_ids_size uint32
	string_ids_off uint32
	type_ids_size uint32
	type_ids_off uint32
	proto_ids_size uint32
	proto_ids_off uint32
	field_ids_size uint32
	field_ids_off uint32
	method_ids_size uint32
	method_ids_off uint32
	class_defs_size uint32
	class_defs_off uint32
	data_size uint32
	data_off uint32
}

type string_data_item struct {
	// ulbe128 len
	data uint8
}

type string_id_item struct {
	string_data_off uint32
}

type type_id_item struct {
	desc_idx uint32 // idx in string ids array
}

type field_id_item struct {
	class_idx uint16 // idx in type_ids array
	type_idx uint16 // idx in type_ids_array
	name_idx uint32   // idx in string ids array
}

type proto_id_item struct {
	shorty_idx uint32      // string ids array idx
	return_type_idx uint16 // type_ids array idx
	pad uint16
	parameters_off uint32  // file offset to parameters type -> type_list -> type_item
}

type method_id_item struct {
	class_idx uint16 // idx in type_ids array
	proto_idx uint16 // idx in type_ids_array
	name_idx uint32   // idx in string ids array
}

type type_item struct {
	type_idx uint16 // type ids array idx
}

type type_list struct {
	size uint32
	list type_item
	//list [1]type_item
}

type class_def struct {
	class_idx uint16 // idx to type_ids_array
	pad1 uint16
	access_flags uint32
	superclass_idx uint16
	pad2 uint16
	interfaces_off uint32 // offset to type list
	source_file_idx uint32 // idx in string_ids_array
	annotation_off uint32 // file offset to annotation_dirtectory_item
	class_data_off uint32 // file offset to class_data_item
	static_values_off uint32 // file offset to EncodedArray, store value of static field if set
}

//Encoded format
type class_data_item struct {
	// uleb128 static fields_size;
	// uleb128 instance_fields_size;
	// uleb128 direct_methods_size;
	// uleb128 virtual_methods_size;
	// struct encoded_field[] static_fields;
	// struct encoded_field[] instace_fields;
	// struct encoded_method[] static_methods;
	// struct encoded_method[] instance_methods;
}

type encoded_field struct {
	// uleb128 field_idx_off
	// uleb128 access_flags
}

type encoded_method struct {
	// uleb128 method_idx_diff;
	// uleb128 access_flags;
	// uleb128 code_off;  // file offset to code_item
};

type decoded_class_data_item_header struct {
	static_fields_size uint32
	instance_fields_size uint32
	static_methods_size uint32
	instance_methods_size uint32
}

type decoded_class_field struct {
	// for first element, delta is index of field_id_item array
	// for others, delta is diff with precede element
	field_idx_delta uint32
	access_flags uint32
}

type decoded_class_method struct {
	method_idx_delta uint32
	access_flags uint32
	code_off uint32     // file offset to code item
}

type try_item struct {
	start_addr uint32
	insn_count uint16
	handler_off uint16
}

type encoded_type_addr_pair struct {
	// uleb128 type_idx;
	// uleb128 addr;
}

type encoded_catch_handler struct {
	// sleb128 size;
	handlers [1]encoded_type_addr_pair
	// uleb128 catch_all_addr;
}

type encoded_catch_handler_list struct {
	// uleb128 handlers_size;
	list [1]encoded_catch_handler
}

type code_item struct {
	register_size uint16
	ins_size uint16
	outs_size uint16
	tries_size uint16
	debug_info_off uint32
	insns_size uint32
	insns [1]uint16
	padding uint16
	tries [1]try_item
	handlers encoded_catch_handler_list
}

type code_item_header struct {
	register_size uint16
	ins_size uint16
	outs_size uint16
	tries_size uint16
	debug_info_off uint32
	insns_size uint32
}

var buff []byte
var mmap_ptr *uint8
var mmap_uintptr uintptr
var dex_header *header_item
//---------------------------------------------------------------------
// Reads a signed LEB128 value, updating the given pointer to point
// just past the end of the read value. This function tolerates
// non-zero high-order bits in the fifth encoded byte.
func decode_unsigned_leb128(data *uint8) (result uint32, next *uint8) {
	var cur uint32
	var tmp_ptr uintptr

	tmp_ptr = uintptr(unsafe.Pointer(data))

	result = uint32(*(*uint8)(unsafe.Pointer(tmp_ptr)))
	tmp_ptr++
	if result > 0x7f {
		cur = uint32(*(*uint8)(unsafe.Pointer(tmp_ptr)))
		tmp_ptr++
		result = (result & 0x7f) | ((cur & 0x7f) << 7)
		if cur > 0x7f {
			cur = uint32(*(*uint8)(unsafe.Pointer(tmp_ptr)))
			tmp_ptr++
			result |= (cur & 0x7f) << 14
			if cur > 0x7f {
				cur = uint32(*(*uint8)(unsafe.Pointer(tmp_ptr)))
				tmp_ptr++
				result |= (cur & 0x7f) << 21
				if cur > 0x7f {
					cur = uint32(*(*uint8)(unsafe.Pointer(tmp_ptr)))
					tmp_ptr++
					// we don't check to see if cur is out of range here
					// meaning we tolerate garbage in the four high-order bits
					result |= cur << 28
				}
			}
		}
	}
	return result, (*uint8)(unsafe.Pointer(tmp_ptr))
}

func get_string_len_and_start_pos(string_item *string_id_item) (ret string, len uint32) {
	tmp_ptr := (*uint8)(unsafe.Pointer(mmap_uintptr + uintptr(string_item.string_data_off)))
	len, start := decode_unsigned_leb128(tmp_ptr)

	ba := []byte{}
	c := *start
	for ; c != 0; {
		ba = append(ba, byte(c))
		start = (*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(start))+uintptr(unsafe.Sizeof(c))))
		c = *start
	}

	return string(ba), len
}

func get_string_with_string_idx(index uint32) string {
	var item string_id_item
	string_id_item_ptr := (*string_id_item)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.string_ids_off) + uintptr(index)*unsafe.Sizeof(item)))
	ret, _ := get_string_len_and_start_pos(string_id_item_ptr)
	return ret
}

func get_string_with_type_idx(index uint32) string {
	var item type_id_item
	type_id_item_ptr := (*type_id_item)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.type_ids_off) + uintptr(index)*unsafe.Sizeof(item)))
	return get_string_with_string_idx(type_id_item_ptr.desc_idx) 
}

func print_field_like_dexdump(index uint32, access_flags uint32) {
	var item field_id_item
	field_item_ptr := (*field_id_item)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.field_ids_off) + uintptr(index)*unsafe.Sizeof(item)))

	fmt.Printf("      name          : '%s'\n", get_string_with_string_idx(field_item_ptr.name_idx));
	fmt.Printf("      type          : '%s'\n", get_string_with_type_idx(uint32(field_item_ptr.type_idx)));
	fmt.Printf("      access        : 0x%04x\n", access_flags);
}

func get_method_full_descriptor(proto_id_item_ptr *proto_id_item) string {
	var str_slice []string
	var type_item_ptr *type_item
	var item type_item

	str_slice = append(str_slice, "(")
	if proto_id_item_ptr.parameters_off > 0 {
		types := (*type_list)(unsafe.Pointer(mmap_uintptr + uintptr(proto_id_item_ptr.parameters_off)))
		if types.size > 0 {
			for i := uint32(0); i < types.size; i++ {
				type_item_ptr = (*type_item)(unsafe.Pointer(uintptr(unsafe.Pointer(types)) + unsafe.Offsetof(types.list) + uintptr(i)*unsafe.Sizeof(item)))
				str_slice = append(str_slice, get_string_with_type_idx(uint32(type_item_ptr.type_idx)))
			}
		}
	}
	str_slice = append(str_slice, ")")
	str_slice = append(str_slice, get_string_with_type_idx(uint32(proto_id_item_ptr.return_type_idx)))
	return strings.Join(str_slice, "")
}

func print_code_item(code_item_header_ptr *code_item_header) {
	fmt.Printf("      registers     : %d\n", code_item_header_ptr.register_size);
	fmt.Printf("      ins           : %d\n", code_item_header_ptr.ins_size);
	fmt.Printf("      outs          : %d\n", code_item_header_ptr.outs_size);
	fmt.Printf("      insns size    : %d 16-bit code units\n", code_item_header_ptr.insns_size);
	fmt.Printf("      catches       :\n");
	fmt.Printf("      positions     :\n");
	fmt.Printf("      locals        :\n");
}

func print_method_like_dexdump(index uint32, access_flags uint32, code_off uint32) {
	var method_item method_id_item
	var proto_item proto_id_item

	method_id_item_ptr := (*method_id_item)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.method_ids_off) + uintptr(index)*unsafe.Sizeof(method_item)))
	proto_id_item_ptr := (*proto_id_item)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.proto_ids_off) + uintptr(method_id_item_ptr.proto_idx)*unsafe.Sizeof(proto_item)))

	fmt.Printf("      name          : '%s'\n", get_string_with_string_idx(method_id_item_ptr.name_idx))
	fmt.Printf("      type          : '%s'\n", get_method_full_descriptor(proto_id_item_ptr))
	fmt.Printf("      access        : 0x%04x\n", access_flags)
			
	fmt.Printf("      code          -\n")
	code_item_header_ptr := (*code_item_header)(unsafe.Pointer(mmap_uintptr + uintptr(code_off)))
	print_code_item(code_item_header_ptr)
}

func printClass(index uint32) {
	var t class_def

	var class_data_header decoded_class_data_item_header
	var class_field decoded_class_field
	var class_method decoded_class_method
	var previous_idx uint32

	classdef := (*class_def)(unsafe.Pointer(mmap_uintptr + uintptr(dex_header.class_defs_off) + uintptr(index)*unsafe.Sizeof(t)))
	var class_data_ptr *uint8
	class_data_ptr = (*uint8)(unsafe.Pointer(uintptr(unsafe.Pointer(mmap_ptr)) + uintptr(classdef.class_data_off)))

	var next *uint8
	class_data_header.static_fields_size, next = decode_unsigned_leb128(class_data_ptr)
	class_data_header.instance_fields_size, next = decode_unsigned_leb128(next)
	class_data_header.static_methods_size, next = decode_unsigned_leb128(next)
	class_data_header.instance_methods_size, next = decode_unsigned_leb128(next)

	fmt.Printf("  Class descriptor  : '%s'\n", get_string_with_type_idx(uint32(classdef.class_idx)))
	fmt.Printf("  Access flags      : 0x%04x\n", classdef.access_flags)
	fmt.Printf("  Superclass        : '%s'\n", get_string_with_type_idx(uint32(classdef.superclass_idx)))
	fmt.Printf("  Interfaces        -\n")

	fmt.Printf("  Static fields     -\n")
	previous_idx = 0
	for i := uint32(0); i < class_data_header.static_fields_size; i++ {
		class_field.field_idx_delta, next  = decode_unsigned_leb128(next)
		class_field.access_flags, next = decode_unsigned_leb128(next)

		field_idx := class_field.field_idx_delta + previous_idx
		previous_idx = field_idx

		fmt.Printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(uint32(classdef.class_idx)));
		print_field_like_dexdump(field_idx, class_field.access_flags)
	}

	fmt.Printf("  Instance fields   -\n")
	previous_idx = 0
	for i := uint32(0); i < class_data_header.instance_fields_size; i++ {
		class_field.field_idx_delta, next  = decode_unsigned_leb128(next)
		class_field.access_flags, next = decode_unsigned_leb128(next)

		field_idx := class_field.field_idx_delta + previous_idx
		previous_idx = field_idx

		fmt.Printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(uint32(classdef.class_idx)));
		print_field_like_dexdump(field_idx, class_field.access_flags)
	}

	fmt.Printf("  Direct Methods    -\n")
	previous_idx = 0
	for i := uint32(0); i < class_data_header.static_methods_size; i++ {
		class_method.method_idx_delta, next  = decode_unsigned_leb128(next)
		class_method.access_flags, next = decode_unsigned_leb128(next)
		class_method.code_off, next = decode_unsigned_leb128(next)

		method_idx := class_method.method_idx_delta + previous_idx
		previous_idx = method_idx

		fmt.Printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(uint32(classdef.class_idx)));
		print_method_like_dexdump(method_idx, class_method.access_flags, class_method.code_off)
	}

	fmt.Printf("  Virtual Methods   -\n")
	previous_idx = 0
	for i := uint32(0); i < class_data_header.instance_methods_size; i++ {
		class_method.method_idx_delta, next  = decode_unsigned_leb128(next)
		class_method.access_flags, next = decode_unsigned_leb128(next)
		class_method.code_off, next = decode_unsigned_leb128(next)

		method_idx := class_method.method_idx_delta + previous_idx
		previous_idx = method_idx

		fmt.Printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(uint32(classdef.class_idx)));
		print_method_like_dexdump(method_idx, class_method.access_flags, class_method.code_off)
	}

	fmt.Printf("  source_file_idx   : %d (%s)\n", classdef.source_file_idx, 
		get_string_with_string_idx(classdef.source_file_idx));	
}

// 8byte magic, 4 for magic ('DEX\n'), 4 for version(end with zero)
func getDexfileVersion(header *header_item) int {
	var s []uint8 = header.magic[4:7]
	ba := make([]byte, 0, 4)
	for _, v := range s {
		ba = append(ba, byte(v))
	}
	ret, err := strconv.Atoi(string(ba));
	if err == nil {
		return ret
	} else {
		fmt.Println("atoi error")
		return -1
	}
}

func appendSliceInplace(slice *[]int, data ...int) {
	*slice = append(*slice, data...)
	fmt.Printf("addr after append2:%p\n", &(*slice)[0])
	fmt.Println("len: ", len(*slice))
	fmt.Println("cap: ", cap(*slice))
	fmt.Println(*slice)
}

func main() {
	var err error
	var dexfile_reader_at *mmap.ReaderAt
	var dexfile_size int

	if len(os.Args) != 2 {
		fmt.Println("usage error")
		return;
	}

	dexfile_reader_at, err = mmap.Open(os.Args[1]);
	if err != nil {
		fmt.Println("open ", os.Args[1], "error")
		return;
	}

	dexfile_size = dexfile_reader_at.Len()
//	fmt.Println("dexfile size: ", dexfile_size)
	
	buff = make([]byte, dexfile_size)
	_, err = dexfile_reader_at.ReadAt(buff, 0)
	mmap_ptr = *(**uint8)(unsafe.Pointer(&buff))
	mmap_uintptr = uintptr(unsafe.Pointer(mmap_ptr))

	dex_header = (*header_item)(unsafe.Pointer(mmap_ptr))
	fmt.Printf("\nDEXFILE MAGIC: ")
	for _, v := range dex_header.magic  {
		fmt.Printf("0x%x ", v)
	}
	fmt.Println("\n\n\nPRINT LIKE DEXDUMP:\n")

	fmt.Printf("Processing '%s'...\n", os.Args[1])
	fmt.Printf("Opened '%s', DEX version '%04d'\n", os.Args[1], getDexfileVersion(dex_header))

	var j uint32 = 0
	for ; j < dex_header.class_defs_size; j++ {
		fmt.Printf("Class #%d\n", j)
		printClass(j)
	}

/*
	slice := make([]int, 1, 10)
	fmt.Printf("addr:%p\n", &slice[0])
	slice = append(slice, 1)
	fmt.Printf("addr after append:%p\n", &slice[0])
	fmt.Println(slice)
	appendSliceInplace(&slice, 4,5,6)
	fmt.Println(slice)
	fmt.Println("len: ", len(slice))
	fmt.Println("cap: ", cap(slice))
*/
	return
}
