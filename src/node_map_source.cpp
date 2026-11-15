#include "node_map.hpp"

#include "error.hpp"

#include <archive.h>
#include <archive_entry.h>
#include <libxml/parser.h>

#include <array>
#include <cstring>
#include <string_view>
#include <strings.h>

using namespace iris;

std::string iris::unzip_xml(std::string_view zip, const char *operation)
{
    std::unique_ptr<archive, decltype(&archive_read_free)> reader(archive_read_new(), archive_read_free);
    require(reader != nullptr, IRIS_NO_MEMORY, operation, "cannot create ZIP reader");
    require(archive_read_support_format_zip(reader.get()) == ARCHIVE_OK &&
            archive_read_open_memory(reader.get(), zip.data(), zip.size()) == ARCHIVE_OK,
            IRIS_GENAPI_ERROR, operation, "cannot open XML ZIP");
    archive_entry *entry = nullptr;
    std::string xml;
    bool found = false;
    int status;
    while ((status = archive_read_next_header(reader.get(), &entry)) == ARCHIVE_OK) {
        const char *path = archive_entry_pathname(entry);
        const char *extension = path ? std::strrchr(path, '.') : nullptr;
        if (archive_entry_filetype(entry) != AE_IFREG || !extension || strcasecmp(extension, ".xml") != 0)
            continue;
        require(!found, IRIS_AMBIGUOUS, operation, "ZIP contains multiple XML documents");
        found = true;
        std::array<char, 8192> bytes;
        for (;;) {
            const auto count = archive_read_data(reader.get(), bytes.data(), bytes.size());
            require(count >= 0, IRIS_GENAPI_ERROR, operation, "cannot read XML ZIP member");
            if (!count) break;
            require(static_cast<size_t>(count) <= max_node_map_xml_bytes - xml.size(),
                    IRIS_OUT_OF_RANGE, operation, "decoded XML exceeds 64 MiB");
            xml.append(bytes.data(), static_cast<size_t>(count));
        }
    }
    require(status == ARCHIVE_EOF, IRIS_GENAPI_ERROR, operation, "cannot read XML ZIP directory");
    require(!xml.empty(), IRIS_NOT_FOUND, operation, "ZIP has no XML document");
    return xml;
}

namespace {

constexpr auto source_operation = "node map XML source";

std::string_view xml_source(iris_node_map &map)
{
    if (map.xml_zipped) {
        map.xml_source = unzip_xml(map.xml_source, source_operation);
        map.xml_zipped = false;
    }
    return map.xml_source;
}

iris_status_t copy_xml(std::string_view source, char *out, size_t *size, const char *operation)
{
    const auto capacity = *size;
    *size = source.size() + 1;
    if (!out) return IRIS_OK;
    if (capacity < *size) return fail(IRIS_BUFFER_TOO_SMALL, operation, "XML output buffer too small");
    std::memcpy(out, source.data(), source.size());
    out[source.size()] = '\0';
    return IRIS_OK;
}

void find_named_element(xmlNode *node, const xmlChar *name, xmlNode *&found)
{
    for (; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;
        for (auto *attribute = node->properties; attribute; attribute = attribute->next) {
            if (attribute->ns || !xmlStrEqual(attribute->name, BAD_CAST "Name")) continue;
            // Read the literal attribute without expanding entity references.
            const auto *value = attribute->children;
            if (value && value->type == XML_TEXT_NODE && !value->next && xmlStrEqual(value->content, name)) {
                require(!found, IRIS_AMBIGUOUS, "feature XML source", "multiple XML elements share this name");
                found = node;
            }
            break;
        }
        find_named_element(node->children, name, found);
    }
}

} // namespace

iris_status_t iris_node_map_xml(iris_node_map_t *map, char *out, size_t *size)
{
    if (!map || !size || (!out && *size)) return invalid(source_operation);
    return api(source_operation, [&] {
        std::lock_guard lock(*map->mutex);
        return copy_xml(xml_source(*map), out, size, source_operation);
    });
}

iris_status_t iris_feature_xml(iris_node_map_t *map, const char *name, char *out, size_t *size)
{
    constexpr auto operation = "feature XML source";
    if (!map || !name || !*name || !size || (!out && *size)) return invalid(operation);
    return api(operation, [&] {
        std::lock_guard lock(*map->mutex);
        const auto source = xml_source(*map);
        constexpr int flags = XML_PARSE_NONET | XML_PARSE_NOERROR | XML_PARSE_NOWARNING;
        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> document(
            xmlReadMemory(source.data(), static_cast<int>(source.size()), nullptr, nullptr, flags), xmlFreeDoc);
        require(document != nullptr, IRIS_GENAPI_ERROR, operation, "cannot parse loaded XML");
        xmlNode *element = nullptr;
        find_named_element(xmlDocGetRootElement(document.get()), BAD_CAST name, element);
        require(element, IRIS_NOT_FOUND, operation, name);

        std::unique_ptr<xmlDoc, decltype(&xmlFreeDoc)> fragment(xmlNewDoc(BAD_CAST "1.0"), xmlFreeDoc);
        require(fragment != nullptr, IRIS_NO_MEMORY, operation, "cannot allocate XML fragment");
        auto *root = xmlDocCopyNode(element, fragment.get(), 1);
        require(root, IRIS_NO_MEMORY, operation, "cannot copy XML element");
        xmlDocSetRootElement(fragment.get(), root);
        require(xmlReconciliateNs(fragment.get(), root) >= 0, IRIS_GENAPI_ERROR, operation,
                "cannot copy XML namespaces");
        std::unique_ptr<xmlBuffer, decltype(&xmlBufferFree)> buffer(xmlBufferCreate(), xmlBufferFree);
        require(buffer != nullptr, IRIS_NO_MEMORY, operation, "cannot allocate XML buffer");
        require(xmlNodeDump(buffer.get(), fragment.get(), root, 0, 0) >= 0,
                IRIS_GENAPI_ERROR, operation, "cannot serialize XML element");
        const auto *text = reinterpret_cast<const char *>(xmlBufferContent(buffer.get()));
        return copy_xml(std::string_view(text, xmlBufferLength(buffer.get())), out, size, operation);
    });
}
