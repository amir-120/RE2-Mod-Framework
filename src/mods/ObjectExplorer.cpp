#include <sstream>
#include <fstream>
#include <forward_list>
#include <deque>
#include <algorithm>
#include <nlohmann/json.hpp>

#include <windows.h>

#include "utility/String.hpp"
#include "utility/Scan.hpp"
#include "utility/Module.hpp"

#include "Genny.hpp"

#include "REFramework.hpp"
#include "ObjectExplorer.hpp"

using json = nlohmann::json;

#ifdef TDB_DUMP_ALLOWED
std::unordered_map<std::string, std::shared_ptr<detail::ParsedType>> g_stypedb{};
std::unordered_map<uint32_t, std::shared_ptr<detail::ParsedType>> g_itypedb{};
std::unordered_map<uint32_t, std::shared_ptr<detail::ParsedType>> g_fqntypedb{};

std::unordered_map<uint32_t, std::shared_ptr<detail::ParsedParams>> g_iparamdb{};
std::unordered_map<uint32_t, std::shared_ptr<detail::ParsedMethod>> g_imethoddb{};
#endif

constexpr std::string_view TYPE_INFO_NAME = "REType";
constexpr std::string_view TYPE_DEFINITION_NAME = "REClassInfo";

std::unordered_set<std::string> g_class_set{};

#if TDB_VER < 69
// via.typeinfo.TypeCode doesn't exist in older games...
std::array<const char*, 84> g_typecode_names{
    "Undefined",
    "Object",
    "Action",
    "Struct",
    "NativeObject",
    "Resource",
    "UserData",
    "Bool",
    "C8",
    "C16",
    "S8",
    "U8",
    "S16",
    "U16",
    "S32",
    "U32",
    "S64",
    "U64",
    "F32",
    "F64",
    "String",
    "MBString",
    "Enum",
    "Uint2",
    "Uint3",
    "Uint4",
    "Int2",
    "Int3",
    "Int4",
    "Float2",
    "Float3",
    "Float4",
    "Float3x3",
    "Float3x4",
    "Float4x3",
    "Float4x4",
    "Half2",
    "Half4",
    "Mat3",
    "Mat4",
    "Vec2",
    "Vec3",
    "Vec4",
    "VecU4",
    "Quaternion",
    "Guid",
    "Color",
    "DateTime",
    "AABB",
    "Capsule",
    "TaperedCapsule",
    "Cone",
    "Line",
    "LineSegment",
    "OBB",
    "Plane",
    "PlaneXZ",
    "Point",
    "Range",
    "RangeI",
    "Ray",
    "RayY",
    "Segment",
    "Size",
    "Sphere",
    "Triangle",
    "Cylinder",
    "Ellipsoid",
    "Area",
    "Torus",
    "Rect",
    "Rect3D",
    "Frustum",
    "KeyFrame",
    "Uri",
    "GameObjectRef",
    "RuntimeType",
    "Sfix",
    "Sfix2",
    "Sfix3",
    "Sfix4",
    "Position",
    "F16",
    "End",
};
#endif

struct BitReader {
    BitReader(void* d)
        : data{(uint8_t*)d} {}

    static constexpr uint64_t make_bitmask(int32_t nbits) {
        if (nbits < 0) {
            return 0;
        }

        if (nbits >= 64) {
            nbits = 63;
        }

        return ((uint64_t)1 << nbits) - 1;
    }

    template<typename T = uint64_t>
    T read(int32_t nbits) {
        constexpr auto CHUNK_BYTES = sizeof(uint8_t);
        constexpr auto CHUNK_BITS = CHUNK_BYTES * 8;

        const auto bidx = (uint8_t)(bit_index % CHUNK_BITS);
        const auto byte_index = (int32_t)floor((float)bit_index / (float)CHUNK_BITS) * CHUNK_BYTES;

        auto window = *(T*)&data[byte_index];
        auto out = (window >> bidx) & (T)make_bitmask(nbits);

        bit_index += nbits;

        return out;
    }

    uint8_t read_byte() { return read<uint8_t>(sizeof(uint8_t) * 8); }
    uint16_t read_short() { return read<uint16_t>(sizeof(uint16_t) * 8); }
    uint32_t read_int() { return read<uint32_t>(sizeof(uint32_t) * 8); }
    uint64_t read_int64() { return read<uint64_t>(sizeof(uint64_t) * 8); }

    void seek(int32_t index) { bit_index = index; }

    uint8_t* data{};
    int32_t bit_index{0};
};

std::vector<std::string> split(const std::string& s, const std::string& token) {
    std::vector<std::string> out{};

    size_t prev = 0;
    for (auto i = s.find(token); i != std::string::npos; i = s.find(token, i + 1)) {
        out.emplace_back(s.substr(prev, i - prev));
        prev = i + 1;
    }

    out.emplace_back(s.substr(prev, std::string::npos));

    return out;
}

genny::Class* class_from_name(genny::Namespace* g, const std::string& class_name) {
    auto namespaces = split(class_name, ".");
    auto new_ns = g;

    if (namespaces.size() > 1) {
        std::string potential_class_name{""};

        bool is_actually_class = false;

        for (auto ns = namespaces.begin(); ns != namespaces.end() - 1; ++ns) {
            if (ns != namespaces.begin()) {
                potential_class_name += ".";
            }

            potential_class_name += *ns;

            if (g_class_set.count(potential_class_name) > 0) {
                class_from_name(g, potential_class_name);
                is_actually_class = true;
            } else {
                new_ns = new_ns->namespace_(*ns);
            }
        }

        if (is_actually_class) {
            auto final_class = class_from_name(g, potential_class_name);

            return final_class->class_(namespaces.back());
        }
    }

    return new_ns->class_(namespaces.back());
}

genny::Enum* enum_from_name(genny::Namespace* g, const std::string& enum_name) {
    auto namespaces = split(enum_name, ".");
    auto new_ns = g;

    if (namespaces.size() > 1) {
        std::string potential_class_name{""};

        bool is_actually_class = false;

        for (auto ns = namespaces.begin(); ns != namespaces.end() - 1; ++ns) {
            if (ns != namespaces.begin()) {
                potential_class_name += ".";
            }

            potential_class_name += *ns;

            if (g_class_set.count(potential_class_name) > 0) {
                class_from_name(g, potential_class_name);
                is_actually_class = true;
            } else {
                new_ns = new_ns->namespace_(*ns);
            }
        }

        if (is_actually_class) {
            auto final_class = class_from_name(g, potential_class_name);

            return final_class->enum_(namespaces.back());
        }
    }

    return new_ns->enum_(namespaces.back());
}

ObjectExplorer::ObjectExplorer()
{
    m_type_name.reserve(256);
    m_object_address.reserve(256);
}

void ObjectExplorer::on_draw_ui() {
    ImGui::SetNextTreeNodeOpen(false, ImGuiCond_::ImGuiCond_Once);

    if (!ImGui::CollapsingHeader(get_name().data())) {
        return;
    }

    if (m_do_init) {
        populate_classes();
        populate_enums();
    }

    if (ImGui::Button("Dump SDK")) {
        generate_sdk();
    }

    auto curtime = std::chrono::system_clock::now();

    // List of globals to choose from
    if (ImGui::CollapsingHeader("Singletons")) {
        if (curtime > m_next_refresh) {
            g_framework->get_globals()->safe_refresh();
            m_next_refresh = curtime + std::chrono::seconds(1);
        }

        // make a copy, we want to sort by name
        auto singletons = g_framework->get_globals()->get_objects();

        // first loop, sort
        std::sort(singletons.begin(), singletons.end(), [](REManagedObject** a, REManagedObject** b) {
            auto a_type = utility::re_managed_object::safe_get_type(*a);
            auto b_type = utility::re_managed_object::safe_get_type(*b);

            if (a_type == nullptr || a_type->name == nullptr) {
                return true;
            }

            if (b_type == nullptr || b_type->name == nullptr) {
                return false;
            }

            return std::string_view{ a_type->name } < std::string_view{ b_type->name };
        });

        // Display the nodes
        for (auto obj : singletons) {
            auto t = utility::re_managed_object::safe_get_type(*obj);

            if (t == nullptr || t->name == nullptr) {
                continue;
            }

            ImGui::SetNextTreeNodeOpen(false, ImGuiCond_::ImGuiCond_Once);

            auto made_node = ImGui::TreeNode(t->name);
            context_menu(*obj);

            if (made_node) {
                handle_address(*obj);
                ImGui::TreePop();
            }
        }
    }

    if (ImGui::CollapsingHeader("Native Singletons")) {
        auto& native_singletons = g_framework->get_globals()->get_native_singletons();

        // Display the nodes
        for (auto t : native_singletons) {
            if (curtime > m_next_refresh_natives) {
                g_framework->get_globals()->safe_refresh_native();
                m_next_refresh_natives = curtime + std::chrono::seconds(1);
            }

            auto obj = utility::re_type::get_singleton_instance(t);

            if (obj != nullptr) {
                handle_type((REManagedObject*)obj, t);
            }
        }
    }

    if (ImGui::CollapsingHeader("Types")) {
        std::vector<uint8_t> fake_type{ 0 };

        for (const auto& name : m_sorted_types) {
            fake_type.clear();

            auto t = get_type(name);

            if (t == nullptr) {
                continue;
            }

            if (t->size >= fake_type.capacity()) {
                fake_type.reserve(t->size);
            }

            handle_type((REManagedObject*)fake_type.data(), t);
        }
    }

    if (m_do_init || ImGui::InputText("Type Name", m_type_name.data(), 256)) {
        m_displayed_types.clear();

        if (auto t = get_type(m_type_name.data())) {
            m_displayed_types.push_back(t);
        }
        else {
            // Search the list for a partial match instead
            for (auto i = std::find_if(m_sorted_types.begin(), m_sorted_types.end(), [this](const auto& a) { return a.find(m_type_name.data()) != std::string::npos; });
                i != m_sorted_types.end();
                i = std::find_if(i + 1, m_sorted_types.end(), [this](const auto& a) { return a.find(m_type_name.data()) != std::string::npos; }))
            {
                if (auto t = get_type(*i)) {
                    m_displayed_types.push_back(t);
                }
            }
        }
    }

    ImGui::InputText("REObject Address", m_object_address.data(), 17, ImGuiInputTextFlags_::ImGuiInputTextFlags_CharsHexadecimal);

    if (m_object_address[0] != 0) {
        handle_address(std::stoull(m_object_address, nullptr, 16));
    }

    std::vector<uint8_t> fake_type{ 0 };

    for (auto t : m_displayed_types) {
        fake_type.clear();
        fake_type.reserve(t->size);
        
        handle_type((REManagedObject*)fake_type.data(), t);
    }

    m_do_init = false;
}

#ifdef TDB_DUMP_ALLOWED
std::shared_ptr<detail::ParsedType> ObjectExplorer::init_type(nlohmann::json& il2cpp_dump, sdk::RETypeDB* tdb, uint32_t i) {
    if (g_itypedb.find(i) != g_itypedb.end()) {
        return g_itypedb[i];
    }

    auto desc = init_type_min(il2cpp_dump, tdb, i);

    g_itypedb[i] = desc;
    g_fqntypedb[desc->t->fqn_hash] = desc;

    return desc;
}

std::string ObjectExplorer::generate_full_name(sdk::RETypeDB* tdb, uint32_t i) {
    if (i == 0 || i >= tdb->numTypes) {
        return "";
    }

    auto& raw_t = (*tdb->types)[i];
    auto full_name = raw_t.get_full_name();

    spdlog::info("{:s}", full_name);

    return full_name;
}

std::shared_ptr<detail::ParsedType> ObjectExplorer::init_type_min(json& il2cpp_dump, sdk::RETypeDB* tdb, uint32_t i) {
    auto& t = (*tdb->types)[i];
    auto br = BitReader{&t};

    auto desc = std::make_shared<detail::ParsedType>();

    desc->t = &t;
    desc->name_space = t.get_namespace();
    desc->name = t.get_name();

    return desc;
}

void ObjectExplorer::export_deserializer_chain(nlohmann::json& il2cpp_dump, sdk::RETypeDB* tdb, REType* t, std::optional<std::string> real_name) {
    /*const auto is_clr_type = (((uint8_t)t->flags >> 5) & 1) != 0;

    if (is_clr_type) {
        return;
    }*/

    std::string full_name{};

    // Export info about native deserializers for the python script
    if (!real_name) {
        auto tdef = (sdk::RETypeDefinition*)t->classInfo;
        full_name = t->classInfo != nullptr ? generate_full_name(tdb, tdef->index) : t->name;
    }
    else {
        full_name = *real_name;
    }

    auto& type_entry = il2cpp_dump[full_name];

    // already done it
    if (type_entry.contains("deserializer_chain") || type_entry.contains("RSZ")) {
        return;
    }

    std::deque<nlohmann::json> chain_raw{};

    for (auto super = t; super != nullptr; super = (sdk::RETypeCLR*)super->super) {
        if (super->fields == nullptr || super->fields->deserializer == nullptr) {
            continue;
        }

        const auto deserializer = super->fields->deserializer;
        const auto deserializer_normalized = get_original_va(deserializer);

        json des_entry{};

        auto tdef = (sdk::RETypeDefinition*)super->classInfo;
        
        des_entry["address"] = (std::stringstream{} << "0x" << std::hex << deserializer_normalized).str();
        des_entry["name"] = super->classInfo != nullptr ? generate_full_name(tdb, tdef->index) : super->name;

        // push in reverse order so it can be parsed easier (parent -> all the way back to this type)
        chain_raw.push_front(des_entry);
    }

    // dont create an empty entry
    if (chain_raw.empty()) {
        return;
    }

    type_entry["deserializer_chain"] = chain_raw;
}
#endif

void ObjectExplorer::generate_sdk() {
    // enums
    auto ref = utility::scan(g_framework->get_module().as<HMODULE>(), "66 C7 40 18 01 01 48 89 05 ? ? ? ?");
    auto& l = *(std::map<uint64_t, REEnumData>*)(utility::calculate_absolute(*ref + 9));

    genny::Sdk sdk{};
    auto g = sdk.global_ns();

    sdk.include("REFramework.hpp");
    sdk.include("sdk/ReClass.hpp");
    sdk.include("cstdint");

    g->type("int8_t")->size(1);
    g->type("int16_t")->size(2);
    g->type("int32_t")->size(4);
    g->type("int64_t")->size(8);
    g->type("uint8_t")->size(1);
    g->type("uint16_t")->size(2);
    g->type("uint32_t")->size(4);
    g->type("uint64_t")->size(8);
    g->type("float")->size(4);
    g->type("double")->size(8);
    g->type("bool")->size(1);
    g->type("char")->size(1);
    g->type("int")->size(4);
    g->type("void")->size(0);
    g->type("void*")->size(8);

    json il2cpp_dump{};

#ifdef TDB_DUMP_ALLOWED
    auto tdb = (sdk::RETypeDB*)g_framework->get_types()->get_type_db();

    // Types
    for (uint32_t i = 0; i < tdb->numTypes; ++i) {
        init_type(il2cpp_dump, tdb, i);
    }

    for (uint32_t i = 0; i < tdb->numTypes; ++i) {
        auto desc = init_type(il2cpp_dump, tdb, i);

        desc->full_name = generate_full_name(tdb, i);
        g_stypedb[desc->full_name] = desc;
    }

    // Finish off initialization of types
    for (uint32_t i = 0; i < tdb->numTypes; ++i) {
        auto desc = init_type(il2cpp_dump, tdb, i);
        auto& t = *desc->t;

        auto tdef = (sdk::RETypeDefinition*)desc->t;

        auto& type_entry = (il2cpp_dump[desc->full_name] = {});
        const auto crc = t.type != nullptr ? t.type->typeCRC : t.type_crc;

        type_entry = {
            {"address", (std::stringstream{} << std::hex << get_original_va(&t)).str()},
            {"id", i},
            {"fqn", (std::stringstream{} << std::hex << t.fqn_hash).str()},
            {"crc", (std::stringstream{} << std::hex << crc).str()},
            {"size", (std::stringstream{} << std::hex << t.size).str()},
        };

        if (tdef->declaring_typeid != 0) {
            desc->owner = init_type(il2cpp_dump, tdb, tdef->declaring_typeid);
        }

        if (tdef->parent_typeid != 0) {
            desc->super = init_type(il2cpp_dump, tdb, tdef->parent_typeid);
            type_entry["parent"] = desc->super->full_name;
        }

        if (auto type_flags_str = get_full_enum_value_name("via.clr.TypeFlag", t.type_flags); !type_flags_str.empty()) {
            type_entry["flags"] = type_flags_str;
        }

        if (desc->t->type != nullptr && desc->t->type->name != nullptr) {
            if (desc->t->type->name != desc->full_name) {
                type_entry["native_typename"] = desc->t->type->name;
            }
        }
    }


    // Initialize RSZ
    // Dont do it in init_type because it calls init_type
    for (uint32_t i = 0; i < tdb->numTypes; ++i) {
        auto pt = init_type(il2cpp_dump, tdb, i);
        auto& t = *pt->t;

        if (t.type == nullptr) {
            continue;
        }

        if (!utility::re_type::is_clr_type(t.type)) {
            export_deserializer_chain(il2cpp_dump, tdb, t.type, pt->full_name);
            continue;
        }

        auto clr_t = (sdk::RETypeCLR*)t.type;
        auto& deserialize_list = clr_t->deserializers;

        for (const auto& sequence : deserialize_list) {
            const auto code = sequence.code;
            const auto size = sequence.size;
            const auto align = sequence.align;
            const auto depth = sequence.depth;
            const auto is_array = sequence.is_array;
            const auto is_static = sequence.is_static;
            
            auto rsz_entry = json{};
            

            rsz_entry["type"] = generate_full_name(tdb, ((sdk::RETypeDefinition*)sequence.native_type)->index);
#if TDB_VER >= 69
            rsz_entry["code"] = get_enum_value_name("via.typeinfo.TypeCode", code);
#else
            rsz_entry["code"] = g_typecode_names[code];
#endif
            rsz_entry["code_id"] = code;
            rsz_entry["align"] = align;
            rsz_entry["size"] = (std::stringstream{} << "0x" << std::hex << (uint32_t)size).str();
            rsz_entry["depth"] = depth;
            rsz_entry["array"] = is_array;
            rsz_entry["static"] = is_static;
            rsz_entry["offset_from_fieldptr"] = (std::stringstream{} << "0x" << std::hex << sequence.offset).str();

            il2cpp_dump[pt->full_name]["RSZ"].emplace_back(rsz_entry);
        }

        // do this because it empty
        if (!il2cpp_dump[pt->full_name].contains("RSZ")) {
            export_deserializer_chain(il2cpp_dump, tdb, t.type, pt->full_name);
        }
    }

    // Methods
    for (uint32_t i = 0; i < tdb->numMethods; ++i) {
        auto& m = (*tdb->methods)[i];

#if TDB_VER >= 69
        auto br = BitReader{&m.data};

        auto type_id = (uint32_t)br.read(18);
        auto impl_id = (uint32_t)br.read(20);
        auto param_list = (uint32_t)br.read(26);
#else
        const auto type_id = (uint32_t)m.declaring_typeid;
        const auto param_list = m.params;
#endif

        if (g_itypedb.find(type_id) == g_itypedb.end()) {
            continue;
        }

        auto& desc = g_itypedb[type_id];

        desc->methods.push_back(&m);

#if TDB_VER >= 69
        auto& impl = (*tdb->methodsImpl)[impl_id];
        desc->method_impls.push_back(&impl);
        const auto name_offset = impl.nameOffset;
#else
        const auto name_offset = m.name_offset;
#endif

        const auto name = Address{ tdb->stringPool }.get(name_offset).as<const char*>();

        // Create an easier to deal with structure
        auto& pm = desc->parsed_methods.emplace_back(std::make_shared<detail::ParsedMethod>());

        pm->m = &m;
        pm->name = name;
        pm->owner = desc;

#if TDB_VER >= 69
        pm->m_impl = &impl;
        const auto vtable_index = impl.vtableIndex;
        const auto impl_flags = impl.implFlags;
        const auto method_flags = impl.flags;
#else
        const auto vtable_index = m.vtable_index;
        const auto impl_flags = m.impl_flags;
        const auto method_flags = m.flags;
#endif

        g_imethoddb[i] = pm;

        //spdlog::info("{:s}.{:s}: 0x{:x}", desc->t->type->name, name, (uintptr_t)m.function);

        auto& type_entry = il2cpp_dump[desc->full_name];
        auto& method_entry = type_entry["methods"][pm->name];

        method_entry["id"] = i;
        method_entry["function"] = (std::stringstream{} << std::hex << get_original_va(m.function)).str();

        if (auto impl_flags_str = get_full_enum_value_name("via.clr.MethodImplFlag", impl_flags); !impl_flags_str.empty()) {
            method_entry["impl_flags"] = impl_flags_str;
        }

        if (auto flags_str = get_full_enum_value_name("via.clr.MethodFlag", method_flags); !flags_str.empty()) {
            method_entry["flags"] = flags_str;
        }

        if (vtable_index >= 0) {
            method_entry["vtable_index"] = vtable_index;
        }

        // Parameters
#if TDB_VER >= 69
        auto param_ids = Address{ tdb->bytePool }.get(param_list).as<REParamList*>();
        const auto num_params = param_ids->numParams;
        const auto invoke_id = param_ids->invokeID;
#else
        auto param_ids = Address{ tdb->bytePool }.get(param_list).as<sdk::REMethodParamDef*>();
        const auto num_params = (uint8_t)m.num_params;
        const auto invoke_id = (uint16_t)m.invoke_id;
#endif

        // Invoke wrapper for arbitrary amount of arguments, so we can just pass it on the VM stack/context as an array
        method_entry["invoke_id"] = invoke_id;

        auto parse_param = [&](uint32_t param_index, bool is_return = false) {
#if TDB_VER >= 69
            auto& p = (*tdb->params)[param_index];

            auto br_p = BitReader{&p};
            const auto attributes_index = (uint16_t)br_p.read_short();
            const auto init_data_index = (uint16_t)br_p.read_short();
            const auto name_index = (uint32_t)br_p.read(30);
            const auto modifier = (uint8_t)br_p.read(2);
            const auto param_type_id = (uint32_t)br_p.read(18);
            const auto flags = (uint16_t)br_p.read(14);
#else
            auto& p = param_ids[param_index];

            const auto param_type_id = (uint32_t)p.param_typeid;
            const auto name_index = p.name_offset;
            const auto flags = p.flags;
#endif

            if (auto it = g_itypedb.find(param_type_id); it == g_itypedb.end()) {
                return json{};
            }

            auto& param_type = g_itypedb[param_type_id];

            auto pdesc = std::make_shared<detail::ParsedParams>();
            g_iparamdb[param_index] = pdesc;

            auto param_name = Address{tdb->stringPool}.get(name_index).as<const char*>();

            pdesc->owner = pm;
            pdesc->type = param_type;
            pdesc->name = param_name;

            if (is_return) {
                pm->return_val = pdesc;
            }
            else {
                pm->params.emplace_back(pdesc);
            }

            auto param_entry = json{
                {"type", pdesc->type->full_name},
                {"name", pdesc->name},
            };

            if (auto param_flags = get_full_enum_value_name("via.clr.ParamFlag", flags); !param_flags.empty()) {
                param_entry["flags"] = param_flags;
            }

            if (auto param_modifier = get_full_enum_value_name("via.clr.ParamModifier", flags); !param_modifier.empty()) {
                param_entry["modifier"] = param_modifier;
            }

            return param_entry;
        };

        // Parse return type
#if TDB_VER >= 69
        method_entry["returns"] = parse_param(param_ids->returnType, true);
#else
        method_entry["returns"] = json{
            {"type", generate_full_name(tdb, m.return_typeid)},
            {"name", ""},
        };
#endif

        // Parse all params
        for (auto f = 0; f < num_params; ++f) {
#if TDB_VER >= 69
            const auto param_index = param_ids->params[f];
            if (param_index >= tdb->numParams) {
                break;
            }
#else
            const auto param_index = f;
#endif

            auto param_entry = parse_param(param_index);

            method_entry["params"].emplace_back(param_entry);
        }
    }

    spdlog::info("FIELDS BEGIN");

    // Fields
    for (uint32_t i = 0; i < tdb->numFields; ++i) {
        auto& f = (*tdb->fields)[i];

#if TDB_VER >= 69
        const auto type_id = (uint32_t)f.declaring_typeid;
        const auto impl_id = (uint32_t)f.impl_id;
        const auto offset = (uint32_t)f.offset;
#else
        const auto type_id = (uint32_t)f.declaring_typeid;
        const auto offset = f.offset;
#endif

        if (g_itypedb.find(type_id) == g_itypedb.end()) {
            continue;
        }

        auto& desc = g_itypedb[type_id];

        desc->fields.push_back(&f);

#if TDB_VER >= 69
        auto& impl = (*tdb->fieldsImpl)[impl_id];
        desc->field_impls.push_back(&impl);


        auto br_impl = BitReader{&impl};

        const auto field_attr_id = (uint16_t)br_impl.read_short();
        const auto field_flags = (uint16_t)br_impl.read_short();
        const auto field_type = (uint32_t)br_impl.read(18);
        const auto init_data_low = (uint16_t)br_impl.read(14);
        const auto name_offset = (uint32_t)br_impl.read(30);
        const auto init_data_high = (uint8_t)br_impl.read(2);
        const auto name = Address{tdb->stringPool}.get(name_offset).as<const char*>();
        const auto init_data_index = init_data_low | (init_data_high << 14);
#else
        const auto name_offset = f.name_offset;
        const auto name = Address{ tdb->stringPool }.get(name_offset).as<const char*>();
        const auto field_type = (uint32_t)f.field_typeid;
        const auto field_flags = f.flags;
        const auto init_data_index = f.init_data_index;
#endif

        // Create an easier to deal with structure
        auto& pf = desc->parsed_fields.emplace_back(std::make_shared<detail::ParsedField>());

        pf->f = &f;
        pf->name = name;
        pf->owner = desc;
        pf->offset_from_fieldptr = offset;
        pf->offset_from_base = pf->offset_from_fieldptr;
        pf->type = g_itypedb[field_type];

#if TDB_VER >= 69
        pf->f_impl = &impl;
#endif

        // Resolve the offset to be from the base class
        if (pf->owner->t->managed_vt != nullptr) {
            const auto field_ptr_offset = Address{ pf->owner->t->managed_vt }.get(-(int32_t)sizeof(void*)).to<int32_t>();

            pf->offset_from_base += field_ptr_offset;
        }
        
        const auto field_type_name = (pf->type != nullptr) ? pf->type->full_name : "";

        //spdlog::info("{:s} {:s}.{:s}: 0x{:x}", field_type_name, desc->t->type->name, name, pf->offset_from_base);

        auto& type_entry = il2cpp_dump[desc->full_name];
        auto& field_entry = type_entry["fields"][pf->name];

        field_entry = {
            {"id", i},
            {"type", field_type_name},
            {"offset_from_base", (std::stringstream{} << "0x" << std::hex << pf->offset_from_base).str()},
            {"offset_from_fieldptr", (std::stringstream{} << "0x" << std::hex << pf->offset_from_fieldptr).str()},
            {"init_data_index", init_data_index}
        };

        if (auto field_flags_str = get_full_enum_value_name("via.clr.FieldFlag", field_flags); !field_flags_str.empty()) {
            field_entry["flags"] = field_flags_str;
        }

        if (init_data_index != 0) {
            const auto init_data_offset = (*tdb->initData)[init_data_index];
            auto init_data = &(*tdb->bytePool)[init_data_offset];

            // WACKY
            if (init_data_offset < 0) {
                init_data = &((uint8_t*)tdb->stringPool)[init_data_offset * -1];
            }

            auto init_data_type = pf->type;
            auto full_name{ init_data_type->full_name };

            // edge case
            if (pf->type->super != nullptr && pf->type->super->full_name == "System.Enum") {
                switch (pf->type->t->size - pf->type->super->t->size) {
                case 1:
                    full_name = "System.Byte";
                    break;
                case 2:
                    full_name = "System.UInt16";
                    break;
                case 4:
                    full_name = "System.UInt32";
                    break;
                case 8:
                    full_name = "System.UInt64";
                    break;
                }
            }

            switch (utility::hash(full_name)) {
            case "System.Boolean"_fnv:
                field_entry["default"] = *(bool*)init_data;
                break;
            case "System.Char"_fnv:
                field_entry["default"] = *(wchar_t*)init_data;
                break;
            case "System.Byte"_fnv:
                field_entry["default"] = *(uint8_t*)init_data;
                break;
            case "System.SByte"_fnv:
                field_entry["default"] = *(int8_t*)init_data;
                break;
            case "System.UInt16"_fnv:
                field_entry["default"] = *(uint16_t*)init_data;
                break;
            case "System.Int16"_fnv:
                field_entry["default"] = *(int16_t*)init_data;
                break;
            case "System.UInt32"_fnv:
                field_entry["default"] = *(uint32_t*)init_data;
                break;
            case "System.Int32"_fnv:
                field_entry["default"] = *(int32_t*)init_data;
                break;
            case "System.UInt64"_fnv:
                field_entry["default"] = *(uint64_t*)init_data;
                break;
            case "System.Int64"_fnv:
                field_entry["default"] = *(int64_t*)init_data;
                break;
            case "System.Single"_fnv:
                field_entry["default"] = *(float*)init_data;
                break;
            case "System.Double"_fnv:
                field_entry["default"] = *(double*)init_data;
                break;
            case "System.String"_fnv:
                field_entry["default"] = (char*)init_data;
                break;
            default:
                field_entry["default"] = "REFRAMEWORK_UNIMPLEMENTED_INIT_TYPE";
                break;
            }
        }
    }
    
    spdlog::info("PROPERTIES BEGIN");

    // properties
    for (uint32_t i = 0; i < tdb->numProperties; ++i) {
        auto& p = (*tdb->properties)[i];

#if TDB_VER >= 69
        auto br = BitReader{&p};

        const auto impl_id = (uint32_t)br.read(20);
        const auto getter_id = (uint32_t)br.read(22);
        const auto setter_id = (uint32_t)br.read(22);
#else
        // well the good thing about this i guess is we don't need to recreate a reclass structure because of the bitfields.
        const auto getter_id = p.getter;
        const auto setter_id = p.setter;
#endif

        std::shared_ptr<detail::ParsedMethod> getter{};
        std::shared_ptr<detail::ParsedMethod> setter{};
        std::shared_ptr<detail::ParsedMethod> prop_method{};

        if (auto it = g_imethoddb.find(getter_id); it != g_imethoddb.end()) {
            prop_method = it->second;
            getter = it->second;
        }
        
        if (auto it = g_imethoddb.find(setter_id); it != g_imethoddb.end()) {
            prop_method = it->second;
            setter = it->second;
        }

        // uhhhh
        if (prop_method == nullptr) {
            continue;
        }

        auto& desc = prop_method->owner;

#if TDB_VER >= 69
        auto& impl = (*tdb->propertiesImpl)[impl_id];
        auto name = Address{tdb->stringPool}.get(impl.nameOffset).as<const char*>();
#else
        auto name = Address{ tdb->stringPool }.get(p.name_offset).as<const char*>();
#endif

        // ha ha
        auto& pp = desc->parsed_props.emplace_back(std::make_shared<detail::ParsedProperty>());

        pp->name = name;
        pp->owner = desc;
        pp->p = &p;
        pp->getter = getter;
        pp->setter = setter;

#if TDB_VER >= 69
        pp->p_impl = &impl;
#endif

        auto getter_name = pp->getter != nullptr ? pp->getter->name : "";
        auto setter_name = pp->setter != nullptr ? pp->setter->name : "";

        //spdlog::info("{:s}.{:s}", desc->name, name);

        auto& type_entry = il2cpp_dump[desc->full_name];

        type_entry["properties"][pp->name] = {
            {"id", i},
            {"getter", getter_name},
            {"setter", setter_name},
        };

        //spdlog::info("{:s} {:s}.{:s}: 0x{:x}", field_type_name, desc->t->type->name, name, pf->offset_from_base);
    }


    // Try and guess what the field names are for the RSZ entries
    for (auto& t : g_itypedb) {
        auto tdef = (sdk::RETypeDefinition*)t.second->t;

        if (tdef == nullptr || tdef->index == 0) {
            continue;
        }

        auto& t_json = il2cpp_dump[t.second->full_name];

        if (!t_json.contains("RSZ")) {
            continue;
        }

        int32_t i = 0;

        for (auto& rsz_entry : t_json["RSZ"]) {
            const auto rsz_offset = std::stoul(std::string{ rsz_entry["offset_from_fieldptr"] }, nullptr, 16);
            auto fieldptr_adjustment = 0;

            auto depth_t = t.second;
            const auto depth = rsz_entry["depth"].get<int32_t>();

            // Get the topmost one because of depth
            for (auto d = 0; d < depth; ++d) {
                if (depth_t->t->managed_vt == nullptr || depth_t->super == nullptr) {
                    break;
                }

                const auto field_ptr = Address{ depth_t->t->managed_vt }.get(-(int32_t)sizeof(void*)).to<int32_t>();

                depth_t = depth_t->super;

                if (depth_t->t->managed_vt == nullptr) {
                    break;
                }

                const auto field_ptr2 = Address{ depth_t->t->managed_vt }.get(-(int32_t)sizeof(void*)).to<int32_t>();

                fieldptr_adjustment += field_ptr - field_ptr2;
            }

            for (auto& f : depth_t->parsed_fields) {
                if (f->offset_from_fieldptr + fieldptr_adjustment == rsz_offset) {
                    rsz_entry["potential_name"] = f->name;
                    break;
                }
            }
        }
    }
#endif

    // First pass, gather all valid class names
    for (const auto& name : m_sorted_types) {
        auto t = get_type(name);

        if (t == nullptr || t->name == nullptr) {
            continue;
        }

#ifdef TDB_DUMP_ALLOWED
        export_deserializer_chain(il2cpp_dump, tdb, t);

        auto& entry = il2cpp_dump[t->name];

        if (!entry.contains("fqn")) {
            entry["fqn"] = (std::stringstream{} << std::hex << t->classIndex).str();
        }

        if (!entry.contains("crc")) {
            entry["crc"] = (std::stringstream{} << std::hex << t->typeCRC).str();
        }
#endif

        if (t->fields == nullptr /*|| t->classInfo == nullptr || utility::re_class_info::get_vm_type(t->classInfo) != via::clr::VMObjType::Object*/) {
            continue;
        }

        // template classes we dont want
        if (std::string{ t->name }.find_first_of("`<>") != std::string::npos) {
            continue;
        }

        g_class_set.insert(t->name);
    }

    for (const auto& name : m_sorted_types) {
        auto t = get_type(name);

        if (t == nullptr || t->name == nullptr) {
            continue;
        }

        if (t->fields == nullptr) {
            continue;
        }

        // template classes we dont want
        if (std::string{ t->name }.find_first_of("`<>") != std::string::npos) {
            continue;
        }

        const auto is_singleton = utility::re_type::is_singleton(t);

        auto c = class_from_name(g, t->name);
        c->size(t->size);

        // make get_type static function
        {
            auto m = c->static_function("get_type_info")->returns(g->type(TYPE_INFO_NAME)->ptr());

            std::stringstream os{};
            os << "return g_framework->get_types()->get(\"" << t->name << "\");";

            m->procedure(os.str());
        }

        // make get_singleton_instance static function
        if (is_singleton) {
            auto m = c->static_function("get_singleton_instance")->returns(c->ptr());

            std::stringstream os{};

            os << "return (" << c->ptr()->get_typename() << ")utility::re_type::get_singleton_instance(get_type_info());";

            m->procedure(os.str());
        }

        auto parent_c = c;

        // generate inheritance
        for (auto super = t->super; super != nullptr; super = super->super) {
            if (super == nullptr || super->name == nullptr) {
                continue;
            }

            if (super->fields == nullptr /*|| super->classInfo == nullptr || utility::re_class_info::get_vm_type(super->classInfo) != via::clr::VMObjType::Object*/) {
                continue;
            }

            // template classes we dont want
            if (std::string{super->name}.find_first_of("`<>") != std::string::npos) {
                continue;
            }

            auto s = class_from_name(g, super->name);
            s->size(super->size);

            parent_c->parent(s);
            parent_c = s;
        }

        auto fields = t->fields;
        auto num_methods = fields->num;
        auto methods = fields->methods;

        // Generate Methods
        if (fields->methods != nullptr) {
            for (auto i = 0; i < num_methods; ++i) {
                auto top = (*methods)[i];

                if (top == nullptr) {
                    continue;
                }

                auto& holder = **top;
                auto descriptor = holder.descriptor;

                if (descriptor == nullptr || descriptor->name == nullptr || descriptor->functionPtr == nullptr) {
                    continue;
                }

                // auto ret = descriptor->returnTypeName != nullptr ? std::string{descriptor->returnTypeName} : std::string{"undefined"};
                std::string ret{"void"};

                auto m = c->function(descriptor->name);

                std::ostringstream os{};
                os << "// " << (descriptor->returnTypeName != nullptr ? descriptor->returnTypeName : "") << "\n";

                if (!is_singleton) {
                    os << "return utility::re_managed_object::call_method((REManagedObject*)this, \"" << descriptor->name << "\", *args);";
                }
                else {
                    os << "return utility::re_managed_object::call_method((REManagedObject*)this, utility::re_type::get_method_desc(get_type_info(), \"" << descriptor->name << "\"), *args);";
                }

                m->param("args")->type(g->type("void**"));
                m->procedure(os.str())->returns(g->type("std::unique_ptr<utility::re_managed_object::ParamWrapper>"));

#ifdef TDB_DUMP_ALLOWED
                json json_params{};

                for (auto f = 0; f < descriptor->numParams; ++f) {
                    auto& param_d = (*descriptor->params)[f];
                    auto param_t = g_itypedb[(*descriptor->params)[f].typeIndex];
                    auto param_typename = (param_t != nullptr && param_d.typeIndex) != 0 ? param_t->full_name : param_d.typeName;

                    json_params.push_back({
                        {"type", param_typename},
                        {"name", param_d.paramName},
                        {"typeindex", param_d.typeIndex}
                    });
                }

                auto return_t = g_itypedb[descriptor->typeIndex];
                auto return_name = (return_t != nullptr && descriptor->typeIndex != 0) ? return_t->full_name : descriptor->returnTypeName;

                il2cpp_dump[t->name]["reflection_methods"][descriptor->name] = {
                    {"function", (std::stringstream{} << "0x" << std::hex << get_original_va(descriptor->functionPtr)).str()},
                    {"returns", return_name},
                    {"params", json_params},
                    {"typeindex", descriptor->typeIndex}
                };
#endif
            }
        }

        // Generate Properties
        if (fields->variables != nullptr && fields->variables != nullptr && fields->variables->data != nullptr) {
            auto descriptors = fields->variables->data->descriptors;

            for (auto i = descriptors; i != descriptors + fields->variables->num; ++i) {
                auto variable = *i;

                if (variable == nullptr) {
                    continue;
                }

                genny::Function* m = nullptr;

                std::ostringstream os{};
                os << "// " << (variable->typeName != nullptr ? variable->typeName : "") << "\n";

                if (utility::reflection_property::is_static(variable)) {
                    m = c->static_function(variable->name);

                    os << "auto tbl = sdk::REGlobalContext::get()->get_static_tbl_for_type(*(uint32_t*)get_type_info()->classInfo & 0x3FFF);\n";
                    os << "if (tbl == nullptr) {\n return nullptr;\n}\n";
                    os << "return utility::re_managed_object::get_field<sdk::DummyData>((REManagedObject*)tbl, utility::re_type::get_field_desc(get_type_info(), \"" << variable->name << "\"));\n";

                    m->procedure(os.str());
                }
                else {
                    m = c->function(variable->name);

                    if (!is_singleton) {
                        os << "return utility::re_managed_object::get_field<sdk::DummyData>((REManagedObject*)this, " << "\"" << variable->name << "\");";
                    }
                    else {
                        os << "return utility::re_managed_object::get_field<sdk::DummyData>((REManagedObject*)this, utility::re_type::get_field_desc(get_type_info(), " << "\"" << variable->name << "\"));";
                    }

                    m->procedure(os.str());
                }

                auto dummy_type = g->namespace_("sdk")->struct_("DummyData")->size(0x100);
                m->returns(dummy_type);

#ifdef TDB_DUMP_ALLOWED
                auto field_t = g_fqntypedb[variable->typeFqn];
                auto field_t_name = (field_t != nullptr && variable->typeFqn != 0) ? field_t->full_name : variable->typeName;

                auto& prop_entry = il2cpp_dump[t->name]["reflection_properties"][variable->name];

                prop_entry = {
                    {"getter", (std::stringstream{} << "0x" << std::hex << get_original_va(variable->function)).str()},
                    {"type", field_t_name},
                };
#endif
            
#ifdef RE8
                // Property attributes
                if (variable->attributes != 0 && variable->attributes != -1) {
                    for (auto attr = (REAttribute*)((uintptr_t)&variable->attributes + variable->attributes); attr != nullptr && !IsBadReadPtr(attr, sizeof(REAttribute)) && attr->info != nullptr; attr = attr->next) {
                        auto type_func = (REType* (*)())attr->info->getType;

                        prop_entry["attributes"].emplace_back(
                            json{ {"name", type_func()->name } }
                        );
                    }
                }
#endif
            }
        }
    }

    for (auto& desc : l) {
        // template classes we dont want
        if (std::string{desc.second.name}.find_first_of("`<>") != std::string::npos) {
            continue;
        }

        auto e = enum_from_name(g, desc.second.name);

        e->type(g->type("uint64_t"));

        for (auto node = desc.second.values; node != nullptr; node = node->next) {
            if (node->name == nullptr) {
                continue;
            }

            e->value(node->name, node->value);
        }
    }

    std::ofstream{ "il2cpp_dump.json" } << il2cpp_dump.dump(4) << std::endl;

    sdk.generate("sdk");
}

void ObjectExplorer::handle_address(Address address, int32_t offset, Address parent, Address real_address) {
    if (!is_managed_object(address)) {
        return;
    }

    if (real_address == nullptr) {
        real_address = address;
    }

    auto object = address.as<REManagedObject*>();
    
    if (parent == nullptr) {
        parent = address;
    }

    bool made_node = false;
    auto is_game_object = utility::re_managed_object::is_a(object, "via.GameObject");


    if (offset != -1) {
        ImGui::SetNextTreeNodeOpen(false, ImGuiCond_::ImGuiCond_Once);

        made_node = stretched_tree_node(parent.get(offset), "0x%X:", offset);
        auto is_hovered = ImGui::IsItemHovered();
        auto additional_text = std::string{};
        auto additional_text2 = std::string{};

        context_menu(real_address);

        if (is_game_object) {
            additional_text = utility::re_string::get_string(address.as<REGameObject*>()->name);
        }
        else {
            // Change name based on VMType
            switch (utility::re_managed_object::get_vm_type(object)) {
            case via::clr::VMObjType::Array:
            {
                auto arr = (REArrayBase*)object;
                std::string name{};
                name += "Array<";
                name += arr->containedType != nullptr ? arr->containedType->type->name : "";
                name += ">";

                additional_text = name;
                break;
            }

            case via::clr::VMObjType::String: {
                additional_text = "String";

                auto t = utility::re_managed_object::get_type(object);

                if (t != nullptr) {
                    auto type_name = std::string{t->name};
                    auto ret = utility::hash(type_name);

                    switch (ret) {
                    case "System.String"_fnv: {
                        auto str = (SystemString*)((uintptr_t)utility::re_managed_object::get_field_ptr(object) - sizeof(REManagedObject));

                        if (str->size > 0) {
                            additional_text2 = utility::narrow(str->data);
                        }

                        break;
                    }
                    default:
                        additional_text2 = "NATIVE_STRING";
                        break;
                    }
                }

                break;
            }
            case via::clr::VMObjType::Delegate: {
                additional_text = "Delegate";
                break;
            }
            case via::clr::VMObjType::ValType:
                additional_text = "ValType";
                break;
            case via::clr::VMObjType::Object: {

                additional_text = object->info->classInfo->type->name;

                auto t = utility::re_managed_object::get_type(object);

                if (t != nullptr) {
                    auto type_name = std::string{t->name};
                    auto ret = utility::hash(type_name);

                    switch (ret) {
                    case "System.String"_fnv: {
                        auto str = (SystemString*)((uintptr_t)utility::re_managed_object::get_field_ptr(object) - sizeof(REManagedObject));

                        if (str->size > 0) {
                            additional_text2 = utility::narrow(str->data);
                        }

                        break;
                    }
                    default:
                        break;
                    }
                }

                break;
            }
            case via::clr::VMObjType::NULL_:
            default:
                additional_text = "NULL_OBJECT";
                break;
            }
        }

        if (is_hovered) {
            make_same_line_text(additional_text, VARIABLE_COLOR_HIGHLIGHT);
            make_same_line_text(additional_text2, { 1.0f, 0.0f, 1.0f, 1.0f });
        }
        else {
            make_same_line_text(additional_text, VARIABLE_COLOR);
            make_same_line_text(additional_text2, { 1.0f, 0.0f, 0.0f, 1.0f });
        }
    }

    if (made_node || offset == -1) {
        if (is_game_object) {
            handle_game_object(address.as<REGameObject*>());
        }

        if (utility::re_managed_object::is_a(object, "via.Component")) {
            handle_component(address.as<REComponent*>());
        }

        handle_type(object, utility::re_managed_object::get_type(object));

        if (utility::re_managed_object::get_vm_type(object) == via::clr::VMObjType::Array) {
            if (ImGui::TreeNode(real_address.get(sizeof(REArrayBase)), "Array Entries")) {
                auto arr = (REArrayBase*)object;
                const bool entry_is_val = ((sdk::RETypeDefinition*)arr->containedType)->get_vm_obj_type() == via::clr::VMObjType::ValType;

                if (entry_is_val) {
                    for (auto i = 0; i < arr->numElements; ++i) {
                        auto elem = utility::re_array::get_element<void*>(arr, i);
                        REManagedObject fake_obj{};
                        fake_obj.info = arr->containedType->parentInfo;

                        auto real_size = arr->containedType->size;

                        std::vector<uint8_t> copied_obj{};
                        copied_obj.resize(real_size);

                        memcpy(&copied_obj[0], &fake_obj, sizeof(REManagedObject));
                        memcpy(&copied_obj[sizeof(REManagedObject)], elem, real_size - sizeof(REManagedObject));
                        real_size = utility::re_managed_object::get_size((REManagedObject*)copied_obj.data());
                        copied_obj.resize(real_size);

                        memcpy(&copied_obj[sizeof(REManagedObject)], elem, real_size - sizeof(REManagedObject));
                        handle_address(copied_obj.data(), i, arr, elem);
                    }
                }
                else {
                    for (auto i = 0; i < arr->numElements; ++i) {
                        handle_address(utility::re_array::get_element<void*>(arr, i), i, arr);
                    }
                }

                ImGui::TreePop();
            }
        }

        if (ImGui::TreeNode(real_address.ptr(), "AutoGenerated Types")) {
            auto type_info = object->info->classInfo->type;
            auto size = utility::re_managed_object::get_size(object);

            for (auto i = (uint32_t)sizeof(void*); i < size; i += sizeof(void*)) {
                auto ptr = Address(object).get(i).to<REManagedObject*>();

                handle_address(ptr, i, real_address.ptr());
            }

            ImGui::TreePop();
        }
    }

    if (made_node && offset != -1) {
        ImGui::TreePop();
    }
}

void ObjectExplorer::handle_game_object(REGameObject* game_object) {
    ImGui::Text("Name: %s", utility::re_string::get_string(game_object->name).c_str());
    make_tree_offset(game_object, offsetof(REGameObject, transform), "Transform");
    make_tree_offset(game_object, offsetof(REGameObject, folder), "Folder");
}

void ObjectExplorer::handle_component(REComponent* component) {
    make_tree_offset(component, offsetof(REComponent, ownerGameObject), "Owner");
    //make_tree_offset(component, offsetof(REComponent, childComponent), "ChildComponent");

    auto children_offset = offsetof(REComponent, childComponent);
    auto children_ptr = Address(component).get(children_offset).to<void*>();

    // Draw children
    if (children_ptr != nullptr) {
        auto made_node = ImGui::TreeNode((uint8_t*)component + children_offset, "0x%X: ChildComponents", children_offset);
        context_menu(children_ptr);

        if (made_node) {
            int32_t count = 0;

            // Iterate the children
            for (auto child = component->childComponent; child != nullptr && child != component; child = child->childComponent) {
                // uh oh
                if (!utility::re_managed_object::is_managed_object(child)) {
                    continue;
                }

                auto child_name = utility::re_managed_object::get_type_name(child);

                made_node = widget_with_context(child, [&]() { return stretched_tree_node(child, "%i", count++); });
                auto tree_hovered = ImGui::IsItemHovered();

                // Draw the variable name with a color
                if (tree_hovered) {
                    make_same_line_text(child_name, VARIABLE_COLOR_HIGHLIGHT);
                }
                else {
                    make_same_line_text(child_name, VARIABLE_COLOR);
                }

                if (made_node) {
                    handle_address(child);
                    ImGui::TreePop();
                }
            }

            ImGui::TreePop();
        }
    }

    make_tree_offset(component, offsetof(REComponent, prevComponent), "PrevComponent");
    make_tree_offset(component, offsetof(REComponent, nextComponent), "NextComponent");
}

void ObjectExplorer::handle_transform(RETransform* transform) {

}

void ObjectExplorer::handle_type(REManagedObject* obj, REType* t) {
    if (obj == nullptr || t == nullptr) {
        return;
    }

    auto count = 0;
    const bool is_singleton = utility::re_type::is_singleton(t);
    const bool is_real_obj = utility::re_managed_object::is_managed_object(obj);
    bool is_top_type_open = false;

    for (auto type_info = t; type_info != nullptr; type_info = type_info->super) {
        auto name = type_info->name;

        if (name == nullptr) {
            continue;
        }

        auto obj_for_widget = (type_info == t && is_singleton && !is_real_obj) ? utility::re_type::get_singleton_instance(type_info) : t;

        auto made_node = widget_with_context(obj_for_widget, [&name]() { return ImGui::TreeNode(name); });

        // top
        if (is_singleton && type_info == t) {
            make_same_line_text("SINGLETON", ImVec4{1.0f, 0.0f, 0.0f, 1.0f});
        }

        if (!made_node) {
            break;
        }

        // top
        if (type_info == t) {
            is_top_type_open = true;
        }

        const auto is_real_object = utility::re_managed_object::is_managed_object(obj);

        // Topmost type
        if (type_info == t && is_real_object) {
            ImGui::Text("Size: 0x%X", utility::re_managed_object::get_size(obj));
        }
        // Super types
        else {
            ImGui::Text("Size: 0x%X", type_info->size);
        }

        ++count;

        // Display type flags
        if (type_info->classInfo != nullptr) {
            if (stretched_tree_node("TypeFlags")) {
                display_enum_value("via.clr.TypeFlag", (int64_t)type_info->classInfo->typeFlags);
                ImGui::TreePop();
            }
        }

        display_reflection_methods(obj, type_info);
        display_reflection_properties(obj, type_info);

        display_native_fields(obj, (sdk::RETypeDefinition*)type_info->classInfo);
    }

    for (auto i = 0; i < count; ++i) {
        // Mimic handle_address
        if (i == count - 1 && is_top_type_open && is_singleton && !is_real_obj) {
            auto singleton_obj = utility::re_type::get_singleton_instance(t);

            if (singleton_obj != nullptr && ImGui::TreeNode(singleton_obj, "AutoGenerated Types")) {
                auto size = t->size;

                for (auto i = (uint32_t)sizeof(void*); i < size; i += sizeof(void*)) {
                    auto ptr = Address(singleton_obj).get(i).to<REManagedObject*>();

                    handle_address(ptr, i, singleton_obj);
                }

                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void ObjectExplorer::display_enum_value(std::string_view name, int64_t value) {
    auto first_found = get_enum_value_name(name, (int64_t)value);

    if (!first_found.empty()) {
        ImGui::Text("%i: ", value);
        ImGui::SameLine();
        ImGui::TextColored(VARIABLE_COLOR, "%s", first_found.c_str());
    }
    // Assume it's a set of flags then
    else {
        ImGui::Text("%i", value);

        std::vector<std::string> names{};

        // Check which bits are set and have enum names
        for (auto i = 0; i < 32; ++i) {
            if (auto bit = (value & ((int64_t)1 << i)); bit != 0) {
                auto value_name = get_enum_value_name(name, bit);

                if (value_name.empty()) {
                    continue;
                }

                names.push_back(value_name);
            }
        }

        // Sort and print names
        std::sort(names.begin(), names.end());
        for (const auto& value_name : names) {
            ImGui::TextColored(VARIABLE_COLOR, "%s", value_name.c_str());
        }
    }
}

void ObjectExplorer::display_reflection_methods(REManagedObject* obj, REType* type_info) {
    volatile auto methods = type_info->fields->methods;

    if (methods == nullptr || *methods == nullptr) {
        return;
    }

    auto num_methods = type_info->fields->num;

    if (ImGui::TreeNode(methods, "Reflection Methods: %i", num_methods)) {
        for (auto i = 0; i < num_methods; ++i) {
            volatile auto top = (*methods)[i];

            if (top == nullptr || *top == nullptr) {
                continue;
            }
            
            auto& holder = **top;
            auto descriptor = holder.descriptor;

            if (descriptor == nullptr || descriptor->name == nullptr) {
                continue;
            }

            auto ret = descriptor->returnTypeName != nullptr ? std::string{ descriptor->returnTypeName } : std::string{ "undefined" };
            auto made_node = widget_with_context(descriptor, [&]() { return stretched_tree_node(descriptor, "%s", ret.c_str()); });
            auto tree_hovered = ImGui::IsItemHovered();

            // Draw the variable name with a color
            if (tree_hovered) {
                make_same_line_text(descriptor->name, VARIABLE_COLOR_HIGHLIGHT);
            }
            else {
                make_same_line_text(descriptor->name, VARIABLE_COLOR);
            }

            if (made_node) {
                ImGui::Text("Address: 0x%p", descriptor);
                ImGui::Text("Function: 0x%p", descriptor->functionPtr);

                if (descriptor->functionPtr != nullptr && ImGui::Button("Attempt to call")) {
                    char poop[0x100]{ 0 };
                    utility::re_managed_object::call_method(obj, descriptor->name, poop);
                }

                auto t2 = get_type(ret);

                if (t2 == nullptr || t2 == type_info) {
                    ImGui::Text("Type: %s", ret.c_str());
                }
                else {
                    std::vector<uint8_t> fake_object{};
                    fake_object.reserve(t2->size);
                    fake_object.clear();

                    handle_type((REManagedObject*)fake_object.data(), t2);
                }

                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void ObjectExplorer::display_reflection_properties(REManagedObject* obj, REType* type_info) {
    if (type_info->fields == nullptr || type_info->fields->variables == nullptr || type_info->fields->variables->data == nullptr) {
        return;
    }

    const auto is_real_object = utility::re_managed_object::is_managed_object(obj);
    auto descriptors = type_info->fields->variables->data->descriptors;

    if (ImGui::TreeNode(type_info->fields, "Reflection Properties: %i", type_info->fields->variables->num)) {
        for (auto i = descriptors; i != descriptors + type_info->fields->variables->num; ++i) {
            auto variable = *i;

            if (variable == nullptr) {
                continue;
            }

            auto local_obj = obj;

            auto made_node = widget_with_context(variable->function, [&]() { return stretched_tree_node(variable, "%s", variable->typeName); });
            auto tree_hovered = ImGui::IsItemHovered();

            // Draw the variable name with a color
            if (tree_hovered) {
                make_same_line_text(variable->name, VARIABLE_COLOR_HIGHLIGHT);
            }
            else {
                make_same_line_text(variable->name, VARIABLE_COLOR);
            }

            // Display the field offset
            if (is_real_object || utility::re_type::is_singleton(type_info)) {
                if (!is_real_object && utility::re_type::is_singleton(type_info)) {
                    local_obj = (REManagedObject*)utility::re_type::get_singleton_instance(type_info);
                }

                if (local_obj != nullptr) {
                    auto offset = get_field_offset(local_obj, variable, type_info);

                    if (offset != 0) {
                        ImGui::SameLine();
                        ImGui::TextColored(ImVec4{1.0f, 0.0f, 0.0f, 1.0f}, "0x%X", offset);
                    }
                }
            }

#ifdef RE8
            const auto allowed = is_real_object || utility::reflection_property::is_static(variable) || utility::re_type::is_singleton(type_info);

            // Set the obj to the static table so we can get static variables
            if (utility::reflection_property::is_static(variable)) {
                const auto type_index = BitReader{&type_info->classInfo->typeIndex}.read<uint32_t>(18);

                local_obj = (REManagedObject*)sdk::REGlobalContext::get()->get_static_tbl_for_type(type_index);

                make_same_line_text("STATIC", ImVec4{ 1.0f, 0.0f, 0.0f, 1.0f });
            }
#else
            const auto allowed = is_real_object || utility::re_type::is_singleton(type_info);
#endif

            // Info about the field
            if (made_node) {
                if (allowed && local_obj != nullptr) {
                    attempt_display_field(local_obj, variable, type_info);
                }

                if (ImGui::TreeNode(variable, "Additional Information")) {
                    ImGui::Text("Address: 0x%p", variable);
                    ImGui::Text("Function: 0x%p", variable->function);

                    // Display type information
                    if (variable->typeName != nullptr) {
                        auto t2 = get_type(variable->typeName);

                        if (t2 == nullptr || t2 == type_info) {
                            ImGui::Text("Type: %s", variable->typeName);
                        }
                        else {
                            std::vector<uint8_t> fake_object{};
                            fake_object.reserve(t2->size);
                            fake_object.clear();

                            handle_type((REManagedObject*)fake_object.data(), t2);
                        }
                    }

                    auto prop_flags = *(sdk::PropertyFlags*)&variable->flags;

                    ImGui::Text("TypeKind: %i (%s)", prop_flags.type_kind, get_enum_value_name("via.reflection.TypeKind", (int64_t)prop_flags.type_kind).c_str());
                    ImGui::Text("Qualifiers: %i", prop_flags.type_qual);
                    ImGui::Text("Attributes: %i", prop_flags.type_attr);
                    ImGui::Text("Size: %i", prop_flags.size);
                    ImGui::Text("ManagedStr: %i", prop_flags.managed_str);
                    ImGui::Text("VarType: %i", variable->variableType);

                    if (variable->staticVariableData != nullptr) {
                        ImGui::Text("GlobalIndex: %i", variable->staticVariableData->variableIndex);
                    }

                    ImGui::TreePop();
                }

                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
}

void ObjectExplorer::display_native_fields(REManagedObject* obj, sdk::RETypeDefinition* tdef) {
#ifdef TDB_DUMP_ALLOWED
    if (tdef == nullptr) {
        return;
    }

    const auto is_real_object = utility::re_managed_object::is_managed_object(obj);
    auto fields = tdef->get_fields();

    if (fields.size() == 0) {
        return;
    }

    bool owner_is_valuetype = tdef->get_vm_obj_type() == via::clr::VMObjType::ValType;

    auto tdb = g_framework->get_types()->get_type_db();

    if (ImGui::TreeNode(fields.begin(), "TDB Fields: %i", fields.size())) {
        for (auto& f : fields) {
#if TDB_VER >= 69
            const auto declaring_typeid = (uint32_t)f.declaring_typeid;
            const auto impl_id = (uint32_t)f.impl_id;
            const auto fieldptr_offset = (uint32_t)f.offset;

            const auto& impl = (*tdb->fieldsImpl)[impl_id];
            const auto name_offset = impl.name_offset;
            const auto field_typeid = impl.field_typeid;
            const auto field_flags = impl.flags;
            const auto init_data_index = impl.init_data_lo | (impl.init_data_hi << 14);
#else
            const auto declaring_type_id = (uint32_t)f.declaring_typeid;
            const auto fieldptr_offset = f.offset;
            const auto name_offset = f.name_offset;
            const auto field_typeid = f.field_typeid;
            const auto field_flags = f.flags;
            const auto init_data_index = f.init_data_index;
#endif
            
            const auto field_type = &(*tdb->types)[field_typeid];
            const auto field_type_name = field_type->get_full_name();
            const auto field_name = Address{ tdb->stringPool }.get(name_offset).as<const char*>();

            auto offset = fieldptr_offset;

            void* data = nullptr;

            bool is_valuetype = false;
            bool is_enum = false;
            bool is_managed_str = false;

            std::string final_type_name = field_type_name;

            // Check the field type's parent
            if (auto fpt = field_type->get_parent_type(); fpt != nullptr) {
                const auto full_name_hash = utility::hash(fpt->get_full_name());

                if (full_name_hash == "System.Enum"_fnv) {
                    is_enum = true;
                    //is_valuetype = true;
                }

                /*if (full_name_hash == "System.ValueType"_fnv) {
                    is_valuetype = true;
                }*/
            }

            is_valuetype = field_type->get_vm_obj_type() == via::clr::VMObjType::ValType;

            std::vector<std::string> postfixes{};

            if ((field_flags & (uint16_t)via::clr::FieldFlag::Static) != 0) {
                postfixes.push_back("STATIC");

                if ((field_flags & (uint16_t)via::clr::FieldFlag::Literal) != 0) {
                    postfixes.push_back("CONST");
                    
                    const auto init_data_offset = (*tdb->initData)[init_data_index];
                    auto init_data = &(*tdb->bytePool)[init_data_offset];

                    // WACKY
                    if (init_data_offset < 0) {
                        init_data = &((uint8_t*)tdb->stringPool)[init_data_offset * -1];
                    }

                    data = init_data;

                    switch (utility::hash(field_type_name)) {
                    case "System.String"_fnv:
                        final_type_name = "c8";
                        break;

                    default:
                        break;
                    }
                }
                else {
                    auto tbl = sdk::REGlobalContext::get()->get_static_tbl_for_type(tdef->index);

                    if (tbl != nullptr) {
                        data = Address{ tbl }.get(fieldptr_offset);

                        if (!is_valuetype) {
                            data = *(void**)data;
                        }
                    }
                }
            }
            else {
                if (!owner_is_valuetype || is_real_object) {
                    offset += tdef->get_fieldptr_offset();
                }

                // Draw the offset
                postfixes.push_back((std::stringstream{} << "0x" << std::uppercase << std::hex << offset).str());

                if (obj != nullptr) {
                    data = Address{ obj }.get(offset);

                    if (!is_valuetype) {
                        data = *(void**)data;
                    }
                }
            }

            is_managed_str = final_type_name == "System.String";

            const auto made_node = widget_with_context(data, [&]() { return stretched_tree_node(&f, "%s", field_type_name.c_str()); });
            const auto tree_hovered = ImGui::IsItemHovered();

            // Draw the variable name with a color
            if (tree_hovered) {
                make_same_line_text(field_name, VARIABLE_COLOR_HIGHLIGHT);
            } else {
                make_same_line_text(field_name, VARIABLE_COLOR);
            }

            // draw stuff after the field like the offset
            for (auto postfix : postfixes) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4{ 1.0f, 0.0f, 0.0f, 1.0f }, postfix.c_str());
            }

            // draw the field data
            if (made_node) {
                display_data(data, data, final_type_name, is_enum, is_managed_str, is_valuetype ? field_type : nullptr);

                ImGui::TreePop();
            }
        }

        ImGui::TreePop();
    }
#else
    return;
#endif
}

void ObjectExplorer::attempt_display_field(REManagedObject* obj, VariableDescriptor* desc, REType* type_info) {
    if (desc->function == nullptr) {
        return;
    }

    auto type_name = std::string{ desc->typeName };
    auto ret = utility::hash(type_name);
    auto get_value_func = (void* (*)(VariableDescriptor*, REManagedObject*, void*))desc->function;

    char raw_data[0x100]{ 0 };

    get_value_func(desc, obj, &raw_data);

    auto prop_flags = *(sdk::PropertyFlags*)&desc->flags;
    const auto is_pointer = prop_flags.managed_str || prop_flags.type_attr == 1 || prop_flags.type_attr == 2;

    if (is_pointer && *(void**)&raw_data == nullptr) {
        ImGui::Text("Null pointer");
        return;
    }

    auto data = (is_pointer ? *(char**)&raw_data : (char*)&raw_data);
    auto field_offset = get_field_offset(obj, desc, type_info);

    void* real_data = nullptr;

    if (field_offset != 0) {
        real_data = Address{ obj }.get(field_offset);
    }

    // i don't understand why the game does this.
    if (prop_flags.managed_str != 0 && prop_flags.type_attr == 2 && data != nullptr) {
        data = *(char**)data;
    }

    display_data(data, real_data, type_name, prop_flags.type_kind == (uint32_t)via::reflection::TypeKind::Enum, prop_flags.managed_str != 0);
}

void ObjectExplorer::display_data(void* data, void* real_data, std::string type_name, bool is_enum, bool managed_str, sdk::RETypeDefinition* override_def) {
    if (data == nullptr) {
        ImGui::Text("Null pointer");
        return;
    }

    constexpr auto min_i8 = std::numeric_limits<int8_t>::min();
    constexpr auto max_i8 = std::numeric_limits<int8_t>::max();
    constexpr auto min_i16 = std::numeric_limits<int16_t>::min();
    constexpr auto max_i16 = std::numeric_limits<int16_t>::max();
    constexpr auto min_int = std::numeric_limits<int32_t>::min();
    constexpr auto max_int = std::numeric_limits<int32_t>::max();
    constexpr auto min_int64 = std::numeric_limits<int64_t>::min();
    constexpr auto max_int64 = std::numeric_limits<int64_t>::max();

    constexpr auto min_u8 = std::numeric_limits<uint8_t>::min();
    constexpr auto max_u8 = std::numeric_limits<uint8_t>::max();
    constexpr auto min_u16 = std::numeric_limits<uint16_t>::min();
    constexpr auto max_u16 = std::numeric_limits<uint16_t>::max();
    constexpr auto min_uint = std::numeric_limits<uint32_t>::min();
    constexpr auto max_uint = std::numeric_limits<uint32_t>::max();
    constexpr auto min_uint64 = std::numeric_limits<uint64_t>::min();
    constexpr auto max_uint64 = std::numeric_limits<uint64_t>::max();

    // uh okay
    constexpr auto min_float = std::numeric_limits<float>::lowest();
    constexpr auto max_float = std::numeric_limits<float>::max();

    constexpr auto min_zero = 0;

    auto make_tree_addr = [this](void* addr) {
        if (widget_with_context(addr, [&]() { return ImGui::TreeNode(addr, "Variable: 0x%p", addr); })) {
            if (is_managed_object(addr)) {
                handle_address(addr);
            }

            ImGui::TreePop();
        }
    };

    // yay for compile time string hashing
    switch (utility::hash(type_name)) {
    case "System.UInt64"_fnv:
    case "size_t"_fnv:
    case "u64"_fnv:
        ImGui::Text("%llu", *(uint64_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(uint64_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_U64, &int_val, 1.0f, &min_uint64, &max_uint64);
        }

        break;
    case "System.Int64"_fnv:
    case "s64"_fnv:
        ImGui::Text("%lli", *(int64_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(int64_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_S64, &int_val, 1.0f, &min_int64, &max_int64);
        }

        break;
    case "System.UInt32"_fnv:
    case "u32"_fnv:
        ImGui::Text("%u", *(uint32_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(uint32_t*)real_data;

            ImGui::DragInt("Set Value", (int*)&int_val, 1.0f, min_uint, max_uint);
        }

        break;
    case "System.Int32"_fnv:
    case "s32"_fnv:
        ImGui::Text("%i", *(int32_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(int32_t*)real_data;

            ImGui::DragInt("Set Value", (int*)&int_val, 1.0f, min_int, max_int);
        }

        break;

    case "System.UInt16"_fnv:
    case "u16"_fnv:
        ImGui::Text("%i", *(uint16_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(uint16_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_U16, &int_val, 1.0f, &min_u16, &max_u16);
        }
        break;

    case "System.Int16"_fnv:
    case "s16"_fnv:
        ImGui::Text("%i", *(int16_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(int16_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_S16, &int_val, 1.0f, &min_i16, &max_i16);
        }
        break;
    case "System.Byte"_fnv:
    case "u8"_fnv:
        ImGui::Text("%u", *(uint8_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(uint8_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_U8, &int_val, 1.0f, &min_u8, &max_u8);
        }

        break;
    case "System.SByte"_fnv:
    case "s8"_fnv:
        ImGui::Text("%i", *(int8_t*)data);

        if (real_data != nullptr) {
            auto& int_val = *(int8_t*)real_data;

            ImGui::DragScalar("Set Value", ImGuiDataType_S8, &int_val, 1.0f, &min_i8, &max_i8);
        }

        break;
    //case "System.Nullable`1<System.Single>"_fnv:
    case "System.Single"_fnv:
    case "f32"_fnv: {
        ImGui::Text("%f", *(float*)data);

        if (real_data != nullptr) {
            auto& float_val = *(float*)real_data;

            ImGui::DragFloat("Set Value", &float_val, 0.01f, min_float, max_float);
        }

        break;
    }
    //case "System.Nullable`1<System.Boolean>"_fnv:
    case "System.Boolean"_fnv:
    case "bool"_fnv:
        if (*(bool*)data) {
            ImGui::Text("true");
        } else {
            ImGui::Text("false");
        }

        if (real_data != nullptr) {
            auto& bool_val = *(bool*)real_data;

            ImGui::Checkbox("Set Value", &bool_val);
        }

        break;
    case "System.Char"_fnv:
    case "c16"_fnv:
        ImGui::Text("%s", utility::narrow((wchar_t*)data).c_str());
        break;
    case "c8"_fnv:
        ImGui::Text("%s", (char*)data);
        break;
    //case "System.Nullable`1<via.vec2>"_fnv:
    case "via.Float2"_fnv:
    case "via.vec2"_fnv: {
        auto vec = (Vector2f*)data;

        ImGui::Text("%f %f", vec->x, vec->y);

        if (real_data != nullptr) {
            auto& vec_val = *(Vector2f*)real_data;

            ImGui::DragFloat2("Set Value", (float*)&vec_val, 0.01f, min_float, max_float);
        }

        break;
    }
    //case "System.Nullable`1<via.vec3>"_fnv:
    case "via.Float3"_fnv:
    case "via.vec3"_fnv: {
        auto vec = (Vector3f*)data;

        if (vec != nullptr) {
            ImGui::Text("%f %f %f", vec->x, vec->y, vec->z);
        }

        if (real_data != nullptr) {
            auto& vec_val = *(Vector3f*)real_data;

            ImGui::DragFloat3("Set Value", (float*)&vec_val, 0.01f, min_float, max_float);
        }

        break;
    }
    case "via.Float4"_fnv:
    case "via.vec4"_fnv:
    case "via.Quaternion"_fnv: {
        auto& quat = *(glm::quat*)data;
        ImGui::Text("%f %f %f %f", quat.x, quat.y, quat.z, quat.w);

        if (real_data != nullptr) {
            auto& vec_val = *(Vector4f*)real_data;

            ImGui::DragFloat4("Set Value", (float*)&vec_val, 0.01f, min_float, max_float);
        }

        break;
    }
    case "System.String"_fnv:
    case "via.string"_fnv: {
        if (managed_str) {
            auto ptr = (SystemString*)data;

            if (ptr != nullptr) {
                ImGui::Text("%s", utility::re_string::get_string(*ptr).c_str());
            } else {
                ImGui::Text("");
            }
        } else {
            ImGui::Text("%s", utility::re_string::get_string(*(REString*)data).c_str());
        }
    } break;
    default: {
        if (is_enum) {
            auto value = *(int32_t*)data;
            display_enum_value(type_name, (int64_t)value);

            if (real_data != nullptr) {
                auto& int_val = *(int32_t*)real_data;

                ImGui::SliderInt("Set Value", &int_val, int_val - 1, int_val + 1);
            }
        } else {
            make_tree_addr(data);

            if (override_def != nullptr && override_def->type != nullptr) {
                handle_type((REManagedObject*)data, override_def->type);
            }
        }

        break;
    }
    }
}

int32_t ObjectExplorer::get_field_offset(REManagedObject* obj, VariableDescriptor* desc, REType* type_info) {
    if (desc->typeName == nullptr || desc->function == nullptr || m_offset_map.find(desc) != m_offset_map.end()) {
        return m_offset_map[desc];
    }

    const auto parent_hash = utility::hash(std::string{ type_info->name });
    const auto name_hash = utility::hash(std::string{ desc->name });
    const auto ret_hash = utility::hash(std::string{ desc->typeName });

    // These usually modify the object state, not what we want.
    if (ret_hash == "undefined"_fnv) {
        return m_offset_map[desc];
    }

    // this function modifies some state which causes an unrecoverable exception
    // not very interested to figure out a generic way to fix it right now
    if (parent_hash == "via.wwise.WwiseDriver"_fnv && name_hash == "AudioInput"_fnv) {
        return m_offset_map[desc];
    }

    if (parent_hash == "via.wwise.WwiseManager"_fnv && name_hash == "ObsOclTargetParamList"_fnv) {
        return m_offset_map[desc];
    }

    if (parent_hash == "via.wwise.WwiseManager"_fnv && name_hash == "MaterialObsOclParamList"_fnv) {
        return m_offset_map[desc];
    }

    static int32_t prev_reference_count = 0;
    auto thread_context = sdk::get_thread_context();

    if (thread_context != nullptr) {
        prev_reference_count = thread_context->referenceCount;
    }

    // Set up our "translator" to throw on any exception,
    // Particularly access violations.
    // Kind of gross but it's necessary for some fields,
    // because the field function may access the thing we modified, which may actually be a pointer,
    // and we need to handle it.
    _set_se_translator([](uint32_t code, EXCEPTION_POINTERS* exc) {
        switch (code) {
        case EXCEPTION_ACCESS_VIOLATION:
        {
            spdlog::info("ObjectExplorer: Attempting to handle access violation.");

            auto thread_context = sdk::get_thread_context();

            // This counter needs to be dealt with, it will end up causing a crash later on.
            // We also need to "destruct" whatever object this is.
            if (thread_context != nullptr) {
                auto& reference_count = thread_context->referenceCount;
                auto count_delta = reference_count - prev_reference_count;

                spdlog::error("{}", reference_count);
                if (count_delta >= 1) {
                    --reference_count;

                    static void* (*context_unhandled_exception_fn)(REThreadContext*) = nullptr;
                    static void* (*context_local_frame_gc_fn)(REThreadContext*) = nullptr;
                    static void* (*context_end_global_frame_fn)(REThreadContext*) = nullptr;

                    // Get our function pointers
                    if (context_unhandled_exception_fn == nullptr) {
                        spdlog::info("Locating funcs");
                        
                        // Version 1
                        //auto ref = utility::scan(g_framework->getModule().as<HMODULE>(), "48 83 78 18 00 74 ? 48 89 D9 E8 ? ? ? ? 48 89 D9 E8 ? ? ? ?");

                        // Version 2 Dec 17th, 2019 game.exe+0x20437C (works on old version too)
                        auto ref = utility::scan(g_framework->get_module().as<HMODULE>(), "48 83 78 18 00 74 ? 48 ? ? E8 ? ? ? ? 48 ? ? E8 ? ? ? ?");

                        if (!ref) {
                            spdlog::error("We're going to crash");
                            break;
                        }

                        context_unhandled_exception_fn = Address{ utility::calculate_absolute(*ref + 11) }.as<decltype(context_unhandled_exception_fn)>();
                        context_local_frame_gc_fn = Address{ utility::calculate_absolute(*ref + 19) }.as<decltype(context_local_frame_gc_fn)>();
                        context_end_global_frame_fn = Address{ utility::calculate_absolute(*ref + 27) }.as<decltype(context_end_global_frame_fn)>();

                        spdlog::info("Context::UnhandledException {:x}", (uintptr_t)context_unhandled_exception_fn);
                        spdlog::info("Context::LocalFrameGC {:x}", (uintptr_t)context_local_frame_gc_fn);
                        spdlog::info("Context::EndGlobalFrame {:x}", (uintptr_t)context_end_global_frame_fn);
                    }

                    // Perform object cleanup that was missed because an exception occurred.
                    if (thread_context->unkPtr != nullptr && thread_context->unkPtr->unkPtr != nullptr) {
                        context_unhandled_exception_fn(thread_context);
                    }

                    context_local_frame_gc_fn(thread_context);
                    context_end_global_frame_fn(thread_context);
                }
                else if (count_delta == 0) {
                    spdlog::info("No fix necessary");
                }
            }
            else {
                spdlog::info("thread context was null. A crash may occur.");
            }
        }
        default:
            break;
        }

        throw std::exception(std::to_string(code).c_str());
    });

    struct BitTester {
        BitTester(uint8_t* old_value)
            : ptr{ old_value }
        {
            old = *old_value;
        }

        ~BitTester() {
            *ptr = old;
        }

        bool is_value_same(const uint8_t* buf) const {
            return buf[0] == ptr[0];
        }

        uint8_t* ptr;
        uint8_t old;
    };

    const auto get_value_func = (void* (*)(VariableDescriptor*, REManagedObject*, void*))desc->function;
    const auto class_size = utility::re_managed_object::is_managed_object(obj) ? utility::re_managed_object::get_size(obj) : type_info->size;
    const auto size = 1;

    // Copy the object so we don't cause a crash by replacing
    // data that's being used by the game
    std::vector<uint8_t> object_copy{};
    object_copy.reserve(class_size);
    memcpy(object_copy.data(), obj, class_size);

    auto first_time = std::chrono::high_resolution_clock::now();

    spdlog::info("{:d} {:s}", GetCurrentThreadId(), desc->name);

    // Compare data
    for (int32_t i = sizeof(REManagedObject); i + size <= (int32_t)class_size; i += 1) {
        auto ptr = object_copy.data() + i;
        bool same = true;

        BitTester tester{ ptr };

        // Compare data twice, first run no modifications,
        // second run, slightly modify the data to double check if it's what we want.
        for (int32_t k = 0; k < 2; ++k) {
            std::array<uint8_t, 0x100> data{ 0 };

            // Attempt to get the field value.
            try {
                get_value_func(desc, (REManagedObject*)object_copy.data(), data.data());
            }
            // Access violation occurred. Good thing we handle it.
            catch (const std::exception&) {
                same = false;
                break;
            }

            // Check if result is the same at our offset.
            same = tester.is_value_same(data.data());

            // Check it by dereferencing it now
            if (!same) {
                const bool deref = desc->variableType == 0 && *(void**)data.data() != nullptr && !IsBadReadPtr(*(void**)data.data(), 1);

                if (deref) {
                    same = tester.is_value_same(deref ? *(uint8_t**)data.data() : data.data());
                }
            }

            if (!same) {
                break;
            }

            // Modify the data for our second run.
            *ptr ^= 1;
        }

        auto elapsed_time = std::chrono::high_resolution_clock::now() - first_time;
        const auto pct = (float)i / (float)class_size;

        // this is taking way too long
        if (pct < 50.0f && elapsed_time >= std::chrono::seconds(1)) {
            break;
        }

        if (same) {
            m_offset_map[desc] = i;
            break;
        }
    }

    return m_offset_map[desc];
}

bool ObjectExplorer::widget_with_context(void* address, std::function<bool()> widget) {
    auto ret = widget();
    context_menu(address);

    return ret;
}

void ObjectExplorer::context_menu(void* address) {
    if (ImGui::BeginPopupContextItem()) {
        if (ImGui::Selectable("Copy")) {
            std::stringstream ss;
            ss << std::hex << (uintptr_t)address;

            ImGui::SetClipboardText(ss.str().c_str());
        }

        // Log component hierarchy to disk
        if (is_managed_object(address) && utility::re_managed_object::is_a((REManagedObject*)address, "via.Component") && ImGui::Selectable("Log Hierarchy")) {
            auto comp = (REComponent*)address;

            for (auto obj = comp; obj; obj = obj->childComponent) {
                auto t = utility::re_managed_object::safe_get_type(obj);

                if (t != nullptr) {
                    if (obj->ownerGameObject == nullptr) {
                        spdlog::info("{:s} ({:x})", t->name, (uintptr_t)obj);
                    }
                    else {
                        auto owner = obj->ownerGameObject;
                        spdlog::info("[{:s}] {:s} ({:x})", utility::re_string::get_string(owner->name), t->name, (uintptr_t)obj);
                    }
                }

                if (obj->childComponent == comp) {
                    break;
                }
            }
        }

        ImGui::EndPopup();
    }
}

void ObjectExplorer::make_same_line_text(std::string_view text, const ImVec4& color) {
    if (text.empty()) {
        return;
    }

    ImGui::SameLine();
    ImGui::TextColored(color, "%s", text.data());
}

void ObjectExplorer::make_tree_offset(REManagedObject* object, uint32_t offset, std::string_view name) {
    auto ptr = Address(object).get(offset).to<void*>();

    if (ptr == nullptr) {
        return;
    }

    auto made_node = ImGui::TreeNode((uint8_t*)object + offset, "0x%X: %s", offset, name.data());

    context_menu(ptr);

    if (made_node) {
        handle_address(ptr);
        ImGui::TreePop();
    }
}

bool ObjectExplorer::is_managed_object(Address address) const {
    return utility::re_managed_object::is_managed_object(address);
}

void ObjectExplorer::populate_classes() {
    auto& type_list = *g_framework->get_types()->get_raw_types();
    spdlog::info("TypeList: {:x}", (uintptr_t)&type_list);

    // I don't know why but it can extend past the size.
    for (auto i = 0; i < type_list.numAllocated; ++i) {
        auto t = (*type_list.data)[i];

        if (t == nullptr || IsBadReadPtr(t, sizeof(REType))) {
            continue;
        }

        if (t->name == nullptr) {
            continue;
        }

        auto name = std::string{ t->name };

        if (name.empty()) {
            continue;
        }

        spdlog::info("{:s}", name);
        m_sorted_types.push_back(name);
        m_types[name] = t;
    }

    std::sort(m_sorted_types.begin(), m_sorted_types.end());
}

void ObjectExplorer::populate_enums() {
    std::ofstream out_file("Enums_Internal.hpp");


    auto ref = utility::scan(g_framework->get_module().as<HMODULE>(), "66 C7 40 18 01 01 48 89 05 ? ? ? ?");
    auto& l = *(std::map<uint64_t, REEnumData>*)(utility::calculate_absolute(*ref + 9));
    spdlog::info("EnumList: {:x}", (uintptr_t)&l);

    spdlog::info("Size: {}", l.size());

    for (auto& elem : l) {
        spdlog::info(" {:x}[ {} {} ]", (uintptr_t)&elem, elem.first, elem.second.name);

        std::string name = elem.second.name;
        std::string nspace = name.substr(0, name.find_last_of("."));
        name = name.substr(name.find_last_of(".") + 1);

        for (auto pos = nspace.find("."); pos != std::string::npos; pos = nspace.find(".")) {
            nspace.replace(pos, 1, "::");
        }


        out_file << "namespace " << nspace << " {" << std::endl;
        out_file << "    enum " << name << " {" << std::endl;

        for (auto node = elem.second.values; node != nullptr; node = node->next) {
            if (node->name == nullptr) {
                continue;
            }

            spdlog::info("     {} = {}", node->name, node->value);
            out_file << "        " << node->name << " = " << node->value << "," << std::endl;

            m_enums.emplace(elem.second.name, EnumDescriptor{ node->name, node->value });
        }

        out_file << "    };" << std::endl;
        out_file << "}" << std::endl;
    }
}

std::string ObjectExplorer::get_full_enum_value_name(std::string_view enum_name, int64_t value) {
    std::string out{};

    // Check which bits are set and have enum names
    for (auto i = 0; i < 32; ++i) {
        if (auto bit = (value & ((int64_t)1 << i)); bit != 0) {
            auto value_name = get_enum_value_name(enum_name, bit);

            if (value_name.empty()) {
                continue;
            }

            if (!out.empty()) {
                out += " | ";
            }

            out += value_name;
        }
    }

    return out;
}

std::string ObjectExplorer::get_enum_value_name(std::string_view enum_name, int64_t value) {
    auto values = m_enums.equal_range(enum_name.data());

    for (auto i = values.first; i != values.second; ++i) {
        if (i->second.value == value) {
            return i->second.name;
        }
    }

    return "";
}

REType* ObjectExplorer::get_type(std::string_view type_name) {
    if (type_name.empty()) {
        return nullptr;
    }
    
    if (auto i = m_types.find(type_name.data()); i != m_types.end()) {
        return i->second;
    }

    return nullptr;
}

uintptr_t ObjectExplorer::get_original_va(void* ptr) {
    if (ptr == nullptr) {
        return 0;
    }

    // Read a small chunk of the executable from disk so we can get the original virtual address for the imagebase
    if (m_module_chunk.empty()) {
        m_module_chunk.resize(5000000);
        std::ifstream{*utility::get_module_path(g_framework->get_module().as<HMODULE>()), std::ifstream::binary}
            .read((char*)m_module_chunk.data(), m_module_chunk.size());
    }

    return *utility::get_imagebase_va_from_ptr(m_module_chunk.data(), g_framework->get_module(), ptr);
}
