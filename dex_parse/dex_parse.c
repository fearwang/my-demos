#include <stdio.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

struct header_item {
	uint8_t  magic[8];
	uint32_t checksum;
	uint8_t signature[20];
	uint32_t file_size;
	uint32_t header_size;
	uint32_t endian_tag;
	uint32_t link_size;
	uint32_t link_off; 
	uint32_t map_off;
	uint32_t string_ids_size; // string_id_item array size
	uint32_t string_ids_off;  // string_id_item array offset
	uint32_t type_ids_size;
	uint32_t type_ids_off;
	uint32_t proto_ids_size;
	uint32_t proto_ids_off;
	uint32_t field_ids_size;
	uint32_t field_ids_off;
	uint32_t method_ids_size;
	uint32_t method_ids_off;
	uint32_t class_defs_size;
	uint32_t class_defs_off;
	uint32_t data_size;
	uint32_t data_off;
};

struct string_data_item {
	// ulbe128 len;
	uint8_t data;
};

struct string_id_item {
	uint32_t string_data_off;
};

struct type_id_item {
	uint32_t desc_idx; // idx in string ids array
};

struct field_id_item {
	uint16_t class_idx; // idx in type_ids_array
	uint16_t type_idx;  // idx in type_ids_array
	uint32_t name_idx;  // idx in string string_ids_array 
};

struct method_id_item {
	uint16_t class_idx; // type_ids_array idx
	uint16_t proto_idx; // proto_ids_array idx
	int32_t name_idx;   // string_ids_array idx
};

struct proto_id_item {
	uint32_t shorty_idx; // string_ids_array idx
	uint16_t return_type_idx; // type_ids_array idx
	uint16_t pad;
	uint32_t parameters_off; // file offset to parameters type -> type_list->type_item
};

struct type_item {
	uint16_t type_idx; // type_ids_array idx
};

struct type_list {
	uint32_t size;
	struct type_item list[1];	
};

struct class_def {
	uint16_t class_idx; // idx to type_ids_array
	uint16_t pad1;
	uint32_t access_flags;
	uint16_t superclass_idx;
	uint16_t pad2;
	uint32_t interfaces_off; // offset to type_list
	uint32_t source_file_idx; // idx in string_ids_array 
	uint32_t annotation_off; // file offset to annotation_directory_item
	uint32_t class_data_off; // file offset to class_data_item
	uint32_t static_values_off; // file offset to EncodedArray, store value of static field if set
};

// Encoded format
struct class_data_item {
	// uleb128 static fields_size;
	// uleb128 instance_fields_size;
	// uleb128 direct_methods_size;
	// uleb128 virtual_methods_size;
	// struct encoded_field[] static_fields;
	// struct encoded_field[] instace_fields;
	// struct encoded_method[] static_methods;
	// struct encoded_method[] instance_methods;
};

struct encoded_field {
	// uleb128 field_idx_diff;
	// uleb128 access_flags;
};

struct encoded_method {
	// uleb128 method_idx_diff;
	// uleb128 access_flags;
	// uleb128 code_off;  // file offset to code_item
};

struct decoded_class_data_item_header {
	uint32_t static_fields_size;
	uint32_t instance_fields_size;
	uint32_t static_methods_size;
	uint32_t instance_methods_size;
};

struct decoded_class_field {
	// for first element, delta is index of field_id_item array
	// for others, delta is diff with precede element
	uint32_t field_idx_delta; // delta of index into fields array for field_id_item array 
	uint32_t access_flags;
};

struct decoded_class_method {
	uint32_t method_idx_delta;
	uint32_t access_flags;
	uint32_t code_off;  // file offset to code_item
};

struct try_item {
	uint32_t start_addr;
	uint16_t insn_count;
	uint16_t handler_off;
};

struct encoded_type_addr_pair {
	// uleb128 type_idx;
	// uleb128 addr;
};

struct encoded_catch_handler {
	// sleb128 size;
	struct encoded_type_addr_pair handlers[1];
	// uleb128 catch_all_addr;
};

struct encoded_catch_handler_list {
	// uleb128 handlers_size;
	struct encoded_catch_handler list[1];
};

struct code_item {
	uint16_t register_size;
	uint16_t ins_size;
	uint16_t outs_size;
	uint16_t tries_size;
	uint32_t debug_info_off;
	uint32_t insns_size;
	uint16_t insns[1];
	uint16_t padding;
	struct try_item tries[1];
	struct encoded_catch_handler_list handlers;
};

struct code_item_header {
	uint16_t register_size;
	uint16_t ins_size;
	uint16_t outs_size;
	uint16_t tries_size;
	uint32_t debug_info_off;
	uint32_t insns_size;
};

uint8_t *mmap_start; // addr to which we map dexfile.
struct header_item *header;

uint32_t decode_unsigned_leb128(uint8_t** data) {
	uint8_t* ptr = *data;
	int result = *(ptr++);
	if (result > 0x7f) {
		int cur = *(ptr++);
		result = (result & 0x7f) | ((cur & 0x7f) << 7);
		if (cur > 0x7f) {
			cur = *(ptr++);
			result |= (cur & 0x7f) << 14;
			if (cur > 0x7f) {
				cur = *(ptr++);
				result |= (cur & 0x7f) << 21;
				if (cur > 0x7f) {
					// Note: We don't check to see if cur is out of range here,
					// meaning we tolerate garbage in the four high-order bits.
					cur = *(ptr++);
					result |= cur << 28;
				}
			}
		}
	}
	*data = ptr;
	return (uint32_t)result;
}

// Reads a signed LEB128 value, updating the given pointer to point
// just past the end of the read value. This function tolerates
// non-zero high-order bits in the fifth encoded byte.
static inline int32_t decode_signed_leb128(const uint8_t** data) {
	const uint8_t* ptr = *data;
	int32_t result = *(ptr++);
	if (result <= 0x7f) {
		result = (result << 25) >> 25;
	} else {
		int cur = *(ptr++);
		result = (result & 0x7f) | ((cur & 0x7f) << 7);
		if (cur <= 0x7f) {
			result = (result << 18) >> 18;
		} else {
			cur = *(ptr++);
			result |= (cur & 0x7f) << 14;
			if (cur <= 0x7f) {
				result = (result << 11) >> 11;
			} else {
				cur = *(ptr++);
				result |= (cur & 0x7f) << 21;
				if (cur <= 0x7f) {
	  				result = (result << 4) >> 4;
				} else {
	  				// Note: We don't check to see if cur is out of range here,
	  				// meaning we tolerate garbage in the four high-order bits.
	  				cur = *(ptr++);
	  				result |= cur << 28;
				}
			}
		}
	}
	*data = ptr;
	return result;
}

uint32_t get_string_len(struct string_id_item *string_item, uint8_t **out_ptr)
{
	uint8_t *tmp_ptr = mmap_start + string_item->string_data_off;
	uint32_t ret = decode_unsigned_leb128(&tmp_ptr);
	*out_ptr = tmp_ptr;
	return ret;
}

char *get_string_with_string_idx(uint32_t string_idx)
{
	struct string_id_item *item = (struct string_id_item *)(mmap_start + header->string_ids_off);
	item += string_idx;

	uint8_t *start_ptr;
	get_string_len(item, &start_ptr);
	return (char *)start_ptr;
}

char *get_string_with_type_idx(uint32_t type_idx)
{
	struct type_id_item *item = (struct type_id_item *)(mmap_start + header->type_ids_off);
	item += type_idx;

	return get_string_with_string_idx(item->desc_idx);
}

void print_string(struct string_id_item *string_item, int newline)
{
	uint8_t *print_ptr;
	//uint32_t string_len = get_string_len(string_item, &print_ptr);
	get_string_len(string_item, &print_ptr);
	printf("%s", (char *)print_ptr);
	if (newline > 0)
		printf("\n");
}

void print_string_idx(uint32_t idx, int newline)
{
	struct string_id_item *string_ids_array = (struct string_id_item *)(mmap_start + header->string_ids_off);
	print_string(string_ids_array + idx, newline);
}

void print_type(uint32_t idx, int newline)
{
	struct type_id_item *type_ids_array = (struct type_id_item *)(mmap_start + header->type_ids_off);
	print_string_idx(type_ids_array[idx].desc_idx, newline);	
}

void print_field(uint32_t idx)
{
	struct field_id_item *field_ids_array = (struct field_id_item *)(mmap_start + header->field_ids_off);
	printf("#%d ", idx);
	printf("class:");
	print_type(field_ids_array[idx].class_idx, 0);
	printf(" type:");
	print_type(field_ids_array[idx].type_idx, 0);
	printf(" name:");
	print_string_idx(field_ids_array[idx].name_idx, 0);
	printf("\n");
}

void print_field_like_dexdump(uint32_t idx, uint32_t access)
{
	struct field_id_item *field_item = (struct field_id_item *)(mmap_start + header->field_ids_off);
	field_item += idx;

	printf("      name          : '%s'\n", get_string_with_string_idx(field_item->name_idx));
	printf("      type          : '%s'\n", get_string_with_type_idx(field_item->type_idx));
	printf("      access        : 0x%04x\n", access);
}

void print_method(uint32_t idx)
{
	struct method_id_item *method_ids_array = (struct method_id_item *)(mmap_start + header->method_ids_off);
	struct method_id_item *target_method_item = &method_ids_array[idx];

	struct proto_id_item *proto_ids_array = (struct proto_id_item *)(mmap_start + header->proto_ids_off);
	struct proto_id_item *target_proto_item = &proto_ids_array[target_method_item->proto_idx];

	printf("#%d \n", idx);
	printf("    short_type:");
	print_string_idx(target_proto_item->shorty_idx, 0);
	printf("\n");
	
	printf("    full_type:");
	print_type(target_proto_item->return_type_idx, 0);
	printf(" ");
	print_type(target_method_item->class_idx, 0);

	printf("::(");

	if (target_proto_item->parameters_off > 0) {
		struct type_list *types = (struct type_list *)(mmap_start + target_proto_item->parameters_off);
		if (types->size > 0) {
			if (types->size > 1) {
				for (uint32_t i = 0; i < types->size-1; i++) {
					struct type_item *item = types->list + i;
					print_type(item->type_idx, 0);
					printf(", ");
				}
			}
			print_type(types->list[types->size-1].type_idx, 0);
		}
	}
	printf(")\n");
}

char* get_method_full_descriptor(struct proto_id_item *proto_item, char *buf)
{
	int ret = 0;
	ret += sprintf(buf, "(");
	if (proto_item->parameters_off > 0) {
		struct type_list *types = (struct type_list *)(mmap_start + proto_item->parameters_off);
		if (types->size > 0) {
			if (types->size > 1) {
				for (uint32_t i = 0; i < types->size-1; i++) {
					struct type_item *item = types->list + i;
					ret += sprintf(buf+ret, "%s", get_string_with_type_idx(item->type_idx));
				}
			}
			ret += sprintf(buf+ret, "%s", get_string_with_type_idx(types->list[types->size-1].type_idx));
		}
	}
	ret += sprintf(buf+ret, ")%s", get_string_with_type_idx(proto_item->return_type_idx));
	return buf;
}

void print_code_item(struct code_item_header *code_item_header)
{
	printf("      registers     : %d\n", code_item_header->register_size);
	printf("      ins           : %d\n", code_item_header->ins_size);
	printf("      outs          : %d\n", code_item_header->outs_size);
	printf("      insns size    : %d 16-bit code units\n", code_item_header->insns_size);
	printf("      catches       :\n");
	printf("      positions     :\n");
	printf("      locals        :\n");
}

void print_method_like_dexdump(uint32_t idx, uint32_t access, uint32_t code_off)
{
	struct method_id_item *method_item = (struct method_id_item *)(mmap_start + header->method_ids_off);
	method_item += idx;
	struct proto_id_item *proto_item = (struct proto_id_item *)(mmap_start + header->proto_ids_off);
	proto_item += method_item->proto_idx;
	char buf[256] = {0};

	printf("      name          : '%s'\n", get_string_with_string_idx(method_item->name_idx));
	printf("      type          : '%s'\n", get_method_full_descriptor(proto_item, buf));
	printf("      access        : 0x%04x\n", access);
	
	printf("      code          -\n");
	print_code_item((struct code_item_header *)(mmap_start + code_off));
}

void print_class(uint32_t idx)
{
	struct class_def *classdef = (struct class_def *)(mmap_start + header->class_defs_off);
	classdef += idx;

	struct decoded_class_data_item_header class_data_header;
	struct decoded_class_field class_field;
	struct decoded_class_method class_method;
	uint32_t previous_idx = 0;

	uint8_t *class_data_ptr = (uint8_t *)(mmap_start + classdef->class_data_off);
	uint8_t *tmp_ptr = class_data_ptr;

	class_data_header.static_fields_size = decode_unsigned_leb128(&tmp_ptr);
	class_data_header.instance_fields_size = decode_unsigned_leb128(&tmp_ptr);
	class_data_header.static_methods_size = decode_unsigned_leb128(&tmp_ptr);
	class_data_header.instance_methods_size = decode_unsigned_leb128(&tmp_ptr);

	// start print
	printf("  Class descriptor  : '%s'\n", get_string_with_type_idx(classdef->class_idx));
	printf("  Access flags      : 0x%04x\n", classdef->access_flags);
	printf("  Superclass        : '%s'\n", get_string_with_type_idx(classdef->superclass_idx));
	printf("  Interfaces        -\n");

	printf("  Static fields     -\n");
	previous_idx = 0;
	for (uint32_t i = 0; i < class_data_header.static_fields_size; i++) {
		class_field.field_idx_delta = decode_unsigned_leb128(&tmp_ptr);
		class_field.access_flags = decode_unsigned_leb128(&tmp_ptr);

		uint32_t field_idx = class_field.field_idx_delta + previous_idx; 
		previous_idx = field_idx;

		printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(classdef->class_idx));
		print_field_like_dexdump(field_idx, class_field.access_flags);
	}

	printf("  Instance fields   -\n");
	previous_idx = 0;
	for (uint32_t i = 0; i < class_data_header.instance_fields_size; i++) {
		class_field.field_idx_delta = decode_unsigned_leb128(&tmp_ptr);
		class_field.access_flags = decode_unsigned_leb128(&tmp_ptr);

		uint32_t field_idx = class_field.field_idx_delta + previous_idx; 
		previous_idx = field_idx;

		printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(classdef->class_idx));
		print_field_like_dexdump(field_idx, class_field.access_flags);
	}
	printf("  Direct Methods    -\n");
	previous_idx = 0;
	for (uint32_t i = 0; i < class_data_header.static_methods_size; i++) {
		class_method.method_idx_delta = decode_unsigned_leb128(&tmp_ptr);
		class_method.access_flags = decode_unsigned_leb128(&tmp_ptr);
		class_method.code_off = decode_unsigned_leb128(&tmp_ptr);

		uint32_t method_idx = class_method.method_idx_delta + previous_idx; 
		previous_idx = method_idx;

		printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(classdef->class_idx));
		print_method_like_dexdump(method_idx, class_method.access_flags, class_method.code_off);
	}

	printf("  Virtual Methods   -\n");
	previous_idx = 0;
	for (uint32_t i = 0; i < class_data_header.instance_methods_size; i++) {
		class_method.method_idx_delta = decode_unsigned_leb128(&tmp_ptr);
		class_method.access_flags = decode_unsigned_leb128(&tmp_ptr);
		class_method.code_off = decode_unsigned_leb128(&tmp_ptr);

		uint32_t method_idx = class_method.method_idx_delta + previous_idx; 
		previous_idx = method_idx;

		printf("    #%d              : (in %s)\n", i, get_string_with_type_idx(classdef->class_idx));
		print_method_like_dexdump(method_idx, class_method.access_flags, class_method.code_off);
	}

	printf("  source_file_idx   : %d (%s)\n", classdef->source_file_idx,
		   get_string_with_string_idx(classdef->source_file_idx));
}

// 8byte magic. 4 for magic, 4 for version
uint32_t get_dexfile_version(struct header_item *header)
{
	return atoi((char *)&header->magic[4]);
}

int main(int argc, char *argv[])
{
	if (argc != 2)
		return -1;

	char *dexfile_path = argv[1];
	int dexfile_fd = open(dexfile_path, O_RDONLY);
	struct stat sb;
	stat(dexfile_path, &sb);
	mmap_start = (uint8_t *)mmap(NULL, sb.st_size >> 1, PROT_READ, MAP_SHARED, dexfile_fd, 0);
	if (mmap_start == MAP_FAILED) {
		printf("map error\n");
		return -1;					
	}

	header = (struct header_item *)mmap_start;

	printf("\n");

	printf("DEXFILE MAGIC: ");
	for (int i = 0; i < 8; i++) {
		printf ("0x%x ", header->magic[i]);
	}
	printf("\n");
	/*
	printf("DEXFILE VERSION: %d\n", get_dexfile_version(header));

	printf("\nPRINT STRINGS: \n");
	for (uint32_t i = 0; i < header->string_ids_size; i++) {
		print_string_idx(i, 1);
	}
	
	printf("\nPRINT TYPE: \n");
	for (uint32_t i = 0; i < header->type_ids_size; i++) {
		print_type(i, 1);
	}

	printf("\nPRINT FIELD:\n");
	for (uint32_t i = 0; i < header->field_ids_size; i++) {
		print_field(i);
	}

	printf("\nPRINT METHOD:\n");
	for (uint32_t i = 0; i < header->method_ids_size; i++) {
		print_method(i);
	}
	*/
	printf("\n\nPRINT LIKE DEXDUMP:\n\n");
	printf("Processing '%s'...\n", argv[1]);
	printf("Opened '%s', DEX version '%04d'\n", argv[1], get_dexfile_version(header));
	for (uint32_t i = 0; i < header->class_defs_size; i++) {
		printf("Class #%d\n", i);
		print_class(i);
	}

	return 0;
}
