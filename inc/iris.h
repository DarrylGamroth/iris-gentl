#ifndef IRIS_H
#define IRIS_H

/**
 * @file iris.h
 * @brief Iris GenTL C API for camera discovery, control, and acquisition.
 *
 * The declarations have C linkage in C++ and use the Linux x86-64 ABI.
 * Public declarations and binary layouts may change during development.
 * All strings are NUL-terminated byte strings. Feature names and enumeration
 * symbols are case-sensitive. Creation outputs are NULL on failure.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** ABI major defined by this header. Development builds may share this value
 * across incompatible changes. */
#define IRIS_ABI_MAJOR 1
/**
 * @brief Report the ABI major of the loaded library.
 * @return The ABI major reported by the loaded library.
 */
uint32_t iris_abi_major(void);

/** Owns open devices and provides access to discovery. */
typedef struct iris_context iris_context_t;
/** Owns an immutable discovery snapshot. */
typedef struct iris_device_list iris_device_list_t;
/** Owns one open GenTL device. */
typedef struct iris_device iris_device_t;
/** Owns one open GenTL data stream. */
typedef struct iris_stream iris_stream_t;
/** Borrowed GenApi node map for a GenTL module or remote device. */
typedef struct iris_node_map iris_node_map_t;

/** Operation result. Zero is success, positive values are receive outcomes,
 * and negative values have a diagnostic available from iris_last_error(). */
typedef int32_t iris_status_t;
enum {
    IRIS_OK = 0,
    IRIS_NO_FRAME = 1,
    IRIS_TIMEOUT = 2,
    IRIS_STOPPED = 3,
    IRIS_NO_EVENT = 4,
    IRIS_INVALID_ARGUMENT = -1,
    IRIS_NOT_FOUND = -2,
    IRIS_AMBIGUOUS = -3,
    IRIS_NOT_SUPPORTED = -4,
    IRIS_ACCESS_DENIED = -5,
    IRIS_BUSY = -6,
    IRIS_DISCONNECTED = -7, /* Reserved. GenTL I/O errors do not establish disconnection. */
    IRIS_BUFFER_TOO_SMALL = -8,
    IRIS_OUT_OF_RANGE = -9,
    IRIS_WRONG_TYPE = -10,
    IRIS_INVALID_STATE = -11,
    IRIS_NO_MEMORY = -12,
    IRIS_PRODUCER_ERROR = -13,
    IRIS_GENAPI_ERROR = -14,
    IRIS_IO_ERROR = -15
};

/** Thread-local error details. String members are borrowed. */
typedef struct {
    const char *operation;       /**< Operation that reported the error. */
    const char *message;         /**< Human-readable diagnostic. */
    int32_t native_code;         /**< Original numeric code when available. */
    uint32_t native_code_valid;  /**< Nonzero when native_code is available. */
} iris_error_t;

/**
 * @brief Read the calling OS thread's last error diagnostic.
 *
 * Call immediately after a negative status. Copy the borrowed strings before
 * calling another Iris function on this thread. The numeric code is meaningful
 * only when native_code_valid is nonzero. Any Iris call may change the
 * diagnostic, including calls that return success.
 *
 * @param[out] error Diagnostic destination. NULL is allowed and has no effect.
 */
void iris_last_error(iris_error_t *error);
/**
 * @brief Create a context for discovery and device ownership.
 *
 * One context can have devices open through several CTIs at the same time.
 *
 * @param[out] context Receives the new context. Close it with iris_context_close().
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_context_create(iris_context_t **context);
/**
 * @brief Close a context after closing all its devices.
 *
 * @param[in,out] context Pointer to the handle. Cleared on success and preserved on failure.
 * @return IRIS_OK, IRIS_BUSY for live devices, or a negative status for another error.
 */
iris_status_t iris_context_close(iris_context_t **context);

/** Discovery snapshots are immutable. Strings remain valid until list close.
 * NULL marks unavailable descriptive metadata. The first three strings form
 * the exact device identity and can be copied for later use. */
typedef struct {
    const char *producer_path;  /**< Canonical absolute CTI path. */
    const char *interface_id;   /**< GenTL interface ID. */
    const char *device_id;      /**< GenTL device ID within the interface. */
    const char *serial_number;  /**< Reported serial number, if available. */
    const char *vendor;         /**< Reported vendor name, if available. */
    const char *model;          /**< Reported model name, if available. */
    const char *display_name;   /**< Reported display name, if available. */
    const char *tl_type;        /**< Reported transport-layer type, if available. */
} iris_device_info_t;

/**
 * @brief Discover devices from one GenTL Producer.
 *
 * A bare filename searches GENICAM_GENTL64_PATH. Multiple distinct matching
 * files yield IRIS_AMBIGUOUS.
 *
 * @param[in] context Open context.
 * @param[in] producer Absolute CTI path or bare CTI filename.
 * @param[in] update_timeout_ms Timeout in milliseconds for each GenTL list update.
 * @param[out] list Receives the immutable snapshot. Close it with iris_device_list_close().
 * @return IRIS_OK even for an empty snapshot, or a negative status.
 */
iris_status_t iris_discover(iris_context_t *context, const char *producer,
                            uint64_t update_timeout_ms, iris_device_list_t **list);
/**
 * @brief Count the entries in a discovery snapshot.
 *
 * @param[in] list Discovery snapshot or NULL.
 * @return The entry count. NULL returns zero.
 */
size_t iris_device_list_count(const iris_device_list_t *list);
/**
 * @brief Read a device entry from a discovery snapshot.
 *
 * @param[in] list Discovery snapshot.
 * @param[in] index Zero-based entry index.
 * @param[out] info Receives metadata with strings borrowed until the snapshot closes.
 * @return IRIS_OK or IRIS_INVALID_ARGUMENT for a missing argument or invalid index.
 */
iris_status_t iris_device_list_get(const iris_device_list_t *list, size_t index,
                                   iris_device_info_t *info);
/**
 * @brief Destroy a discovery snapshot and invalidate its borrowed strings.
 *
 * @param[in,out] list Pointer to the list handle, cleared on return. NULL and an empty handle are allowed.
 */
void iris_device_list_close(iris_device_list_t **list);

/** Requested GenTL device access. */
typedef int32_t iris_device_access_t;
enum {
    IRIS_DEVICE_READ_ONLY = 1,
    IRIS_DEVICE_CONTROL = 2,
    IRIS_DEVICE_EXCLUSIVE = 3
};

/** Exact ID mode requires interface_id and device_id. Serial mode requires
 * serial_number and accepts an optional interface_id to narrow the search.
 * Exactly one mode is required. Serial mode uses discovery metadata.
 * Open revalidates the selection. Strings are needed only during the call. */
typedef struct {
    const char *producer;       /**< CTI path or filename accepted by iris_discover(). */
    const char *interface_id;   /**< Required for exact ID. Optional for serial mode. */
    const char *device_id;      /**< Required for exact ID. NULL for serial mode. */
    const char *serial_number;  /**< Required for serial mode. NULL for exact ID. */
} iris_device_selector_t;

/**
 * @brief Open one device selected by exact identity or serial number.
 *
 * Selection is revalidated against the producer. The open device keeps its
 * producer and interface alive.
 *
 * @param[in] context Open context.
 * @param[in] selector CTI and one selection mode. Input strings are needed only during this call.
 * @param[in] access Requested GenTL device access.
 * @param[in] update_timeout_ms Timeout in milliseconds for each GenTL list update.
 * @param[out] device Receives the new device handle.
 * @return IRIS_OK, IRIS_NOT_FOUND, IRIS_AMBIGUOUS, or another negative status.
 */
iris_status_t iris_device_open(iris_context_t *context,
                               const iris_device_selector_t *selector,
                               iris_device_access_t access,
                               uint64_t update_timeout_ms,
                               iris_device_t **device);

/** Device access, discovery timeout, and optional remote GenApi XML override. */
typedef struct {
    uint64_t update_timeout_ms;  /**< Timeout in milliseconds for each GenTL list update. */
    const char *remote_xml_path;  /**< Local XML or single-XML ZIP path. NULL selects the producer description. */
    iris_device_access_t access; /**< Requested GenTL device access. */
} iris_device_open_options_t;

/**
 * @brief Open a device with optional caller-supplied remote GenApi XML.
 *
 * Relative paths resolve from the working directory. Iris loads the override
 * during open and connects it to the device's remote port. The same remote
 * node map serves feature access, event decoding, and chunk decoding. Only
 * this device's remote map uses the override. An unreadable or invalid
 * override fails the open. The file and input strings are needed only during
 * the call.
 *
 * @param[in] context Open context.
 * @param[in] selector CTI and device selection mode.
 * @param[in] options Device access, timeout, and optional XML or ZIP path.
 * @param[out] device Receives the new device handle. Cleared on failure.
 * @return IRIS_OK, IRIS_IO_ERROR for an unavailable file, IRIS_GENAPI_ERROR
 *         for rejected XML or port connection, or another negative status.
 */
iris_status_t iris_device_open_with_options(iris_context_t *context,
                                            const iris_device_selector_t *selector,
                                            const iris_device_open_options_t *options,
                                            iris_device_t **device);
/**
 * @brief Close a device and release its producer and interface references.
 *
 * Serialize device and stream control across all sibling streams, including
 * open/close, stream enumeration, node-map access, and start/stop. Quiesce
 * all users before close. Live streams prevent teardown and leave the device
 * usable. Otherwise discard this device's node-map borrows before calling
 * close. A failed teardown leaves a close-only handle for retry. Peer devices
 * retain their own borrows.
 *
 * @param[in,out] device Pointer to the handle. Cleared on success and preserved on failure.
 * @return IRIS_OK, IRIS_BUSY for live streams, or another negative status.
 */
iris_status_t iris_device_close(iris_device_t **device);
/**
 * @brief Count the GenTL data streams available on an open device.
 *
 * @param[in] device Open device.
 * @param[out] count Receives the number of data streams.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_device_stream_count(iris_device_t *device, size_t *count);
/**
 * @brief Open a data stream by its index on the device.
 *
 * @param[in] device Open device.
 * @param[in] index Zero-based stream index.
 * @param[out] stream Receives the new stream handle.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_stream_open(iris_device_t *device, size_t index,
                               iris_stream_t **stream);
/**
 * @brief Close a stopped stream after releasing all borrowed frames.
 *
 * Quiesce all stream users before closing. Retry a failed close with the same
 * handle. A successful close invalidates the stream's borrowed node map.
 *
 * @param[in,out] stream Pointer to the handle. Cleared on success and preserved on failure.
 * @return IRIS_OK, IRIS_BUSY if stop or frame release is still required, or another negative status.
 */
iris_status_t iris_stream_close(iris_stream_t **stream);

/** Node maps are borrowed under the owner close rules. Access is evaluated at
 * each call. Serialize feature operations with start and stop for the
 * associated device. System and interface maps are shared by devices on
 * the same producer or interface. Their feature access is independent of
 * remote device open mode. All numeric values use the feature's units. */
typedef int32_t iris_module_t;
enum {
    IRIS_SYSTEM_MODULE = 1,
    IRIS_INTERFACE_MODULE = 2,
    IRIS_DEVICE_MODULE = 3,
    IRIS_REMOTE_DEVICE = 4
};
/**
 * @brief Borrow the node map for a device-related GenTL module.
 *
 * System and interface maps are shared by devices that use the same module.
 * Follow iris_device_close() for borrow lifetime.
 *
 * @param[in] device Open device.
 * @param[in] module System, interface, local device, or remote device.
 * @param[out] map Receives the borrowed node map.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_device_node_map(iris_device_t *device, iris_module_t module,
                                   iris_node_map_t **map);
/**
 * @brief Borrow the data stream's GenApi node map.
 *
 * @param[in] stream Open stream.
 * @param[out] map Receives the borrowed map, valid until stream close succeeds.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_stream_node_map(iris_stream_t *stream, iris_node_map_t **map);

enum {
    IRIS_XML_HAS_SCHEMA_VERSION = 1u << 0, /**< Producer reported both schema version fields. */
    IRIS_XML_HAS_FILE_VERSION = 1u << 1,  /**< Producer reported all three XML file version fields. */
    IRIS_XML_IS_OVERRIDE = 1u << 2        /**< Caller supplied the remote device's XML file. */
};

/** The XML description selected when Iris loaded a node map. */
typedef struct {
    const char *url;          /**< Producer URL or absolute override file URL, borrowed from the map. */
    uint32_t url_index;      /**< Zero-based producer URL index, or UINT32_MAX for an override. */
    uint32_t indexed;        /**< Nonzero if selected via GCGetPortURLInfo. */
    uint32_t valid;          /**< IRIS_XML_HAS_* version bits and IRIS_XML_IS_OVERRIDE source bit. */
    int32_t schema_major;    /**< GenApi XML schema major version. */
    int32_t schema_minor;    /**< GenApi XML schema minor version. */
    int32_t file_major;      /**< XML description major version. */
    int32_t file_minor;      /**< XML description minor version. */
    int32_t file_subminor;   /**< XML description subminor version. */
} iris_node_map_xml_info_t;

/**
 * @brief Read the selected XML URL and advertised versions.
 * @param[in] map Borrowed node map.
 * @param[out] info Receives the selected source and optional indexed GenTL version metadata.
 *                  The URL is valid for the lifetime of the map.
 * @return IRIS_OK or IRIS_INVALID_ARGUMENT.
 */
iris_status_t iris_node_map_xml_info(const iris_node_map_t *map, iris_node_map_xml_info_t *info);

/**
 * @brief Copy the XML description used to load a node map.
 *
 * ZIP descriptions return the XML member's bytes. The copy includes one
 * trailing NUL. The XML retains its original encoding.
 *
 * @param[in] map Borrowed node map.
 * @param[out] xml Destination buffer, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including the added NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL with destination untouched, or a negative status.
 */
iris_status_t iris_node_map_xml(iris_node_map_t *map, char *xml, size_t *size);

/**
 * @brief Invalidate the GenApi cache for every node in a map.
 *
 * This control call takes the feature lock. Later feature queries refresh
 * cached information as needed. It requires no GenTL event subscription.
 *
 * @param[in] map Borrowed node map.
 * @return IRIS_OK or a negative status.
 */
iris_status_t iris_node_map_invalidate(iris_node_map_t *map);

/**
 * @brief Enable or disable GenTL feature invalidation events for a remote map.
 *
 * Repeating the current setting succeeds. Serialize with owner close. Poll
 * the subscription with iris_node_map_poll_invalidation() to apply events.
 * Iris tries the local device handle first and supports producers that
 * register this notification on the remote port.
 *
 * @param[in] map Borrowed remote device node map.
 * @param[in] enabled 1 to register EVENT_FEATURE_INVALIDATE, 0 to unregister it.
 * @return IRIS_OK, IRIS_NOT_SUPPORTED if the producer lacks support, or another negative status.
 */
iris_status_t iris_node_map_enable_invalidation(iris_node_map_t *map, uint32_t enabled);
/**
 * @brief Consume queued feature invalidation events and refresh the map cache.
 *
 * Uses a zero producer event timeout. This operation can allocate and takes
 * the feature lock. An event for a known node invalidates the whole map so
 * subsequent reads refresh backing registers across selector states.
 * An envelope larger than 1 MiB is discarded and reported as IRIS_OUT_OF_RANGE.
 * Discarding an oversized invalidation also invalidates the map.
 *
 * @param[in] map Borrowed node map with invalidation enabled.
 * @param[in] limit Positive maximum event count.
 * @param[out] processed Consumed event count, including names absent from the map and events consumed before a later error.
 * @return IRIS_OK even when the queue is empty, IRIS_INVALID_STATE when disabled, or another negative status.
 */
iris_status_t iris_node_map_poll_invalidation(iris_node_map_t *map, size_t limit, size_t *processed);

/** Maximum number of hexadecimal digits in an event ID accepted by Iris. */
#define IRIS_EVENT_ID_MAX_DIGITS 128

/** ID and payload size for a dequeued GenTL event. */
typedef struct {
    uint64_t numeric_id;                 /**< Numeric event ID when has_numeric_id is nonzero. */
    size_t data_size;                    /**< Bytes delivered to the node map's event ports. */
    uint32_t has_numeric_id;             /**< Whether numeric_id is valid. */
    char id[IRIS_EVENT_ID_MAX_DIGITS + 1]; /**< NUL-terminated hexadecimal event ID. */
} iris_node_map_event_t;

/**
 * @brief Enable or disable events that deliver data to a GenApi node map.
 *
 * A remote device map subscribes to GenTL EVENT_REMOTE_DEVICE on the local
 * device. System, interface, local device, and stream maps subscribe to
 * EVENT_MODULE on their owner. Shared maps share one subscription. Repeating
 * the current setting succeeds. The camera or module may also require
 * configuring EventSelector and EventNotification. Serialize with owner close.
 *
 * @param[in] map Borrowed node map.
 * @param[in] enabled 1 to subscribe, 0 to unsubscribe and discard queued events.
 * @return IRIS_OK, IRIS_NOT_SUPPORTED if unavailable, or another negative status.
 */
iris_status_t iris_node_map_enable_events(iris_node_map_t *map, uint32_t enabled);

/**
 * @brief Try to dequeue one GenTL event and deliver it to the GenApi node map.
 *
 * Uses a zero GenTL timeout and does not wait for the Iris feature lock.
 * Producer and GenApi calls can take time, and event polling can allocate.
 * Iris accepts up to 1 MiB each for the producer's event envelope and payload.
 * An oversized envelope is discarded and reported as IRIS_OUT_OF_RANGE.
 * On success the map's matching event features can be read using the usual
 * feature getters. Values can persist from the last event for each ID. Use
 * the returned ID to select the associated features and serialize polling,
 * feature readback, and subscription changes for a consistent per-event read.
 * Events with no matching XML feature still return their IDs. An event can be
 * consumed even if decoding later returns an error.
 *
 * @param[in] map Borrowed node map with events enabled.
 * @param[out] event Receives the event on IRIS_OK. Other returns clear it.
 * @return IRIS_OK, IRIS_NO_EVENT for an empty queue, IRIS_BUSY for lock contention,
 *         IRIS_INVALID_STATE when disabled, or another negative status.
 */
iris_status_t iris_node_map_try_get_event(iris_node_map_t *map, iris_node_map_event_t *event);

/** Owns the payload and decoded feature values of one event. */
typedef struct iris_event_snapshot iris_event_snapshot_t;

enum {
    IRIS_EVENT_HAS_HOST_TIMESTAMP_NS = 1u << 0, /**< Host monotonic receive timestamp is valid. */
    IRIS_EVENT_HAS_DEVICE_TIMESTAMP = 1u << 1   /**< A matching SFNC event timestamp node was read. */
};

/** View of an owned event snapshot. Data remains valid until snapshot close. */
typedef struct {
    iris_node_map_event_t event; /**< Event identifier and payload size. */
    const void *data;             /**< Raw GenTL EVENT_DATA_VALUE bytes, or NULL for an empty payload. */
    uint64_t host_timestamp_ns;  /**< CLOCK_MONOTONIC time at successful dequeue. */
    uint64_t device_timestamp;   /**< Unscaled value of the matching Event…Timestamp Integer node. */
    uint32_t valid;              /**< IRIS_EVENT_HAS_* bits. */
} iris_event_snapshot_info_t;

/**
 * @brief Try to receive one event as an owned snapshot.
 *
 * Uses the same subscription, zero GenTL timeout, and try-lock as
 * iris_node_map_try_get_event(). Copies the payload and captures GenApi
 * feature values for the received event ID before releasing the feature lock.
 * This control operation can allocate and invoke GenApi or producer code.
 * A snapshot can outlive the map and subsequent event polls.
 *
 * @param[in] map Borrowed node map with events enabled.
 * @param[out] snapshot Receives the owned event. Cleared on other returns.
 * @return IRIS_OK, IRIS_NO_EVENT, IRIS_BUSY, or another negative status.
 */
iris_status_t iris_node_map_try_get_event_snapshot(iris_node_map_t *map, iris_event_snapshot_t **snapshot);

/**
 * @brief Read a snapshot's identifier, timestamps, and raw payload view.
 *
 * The device timestamp is present when exactly one matching readable
 * Event…Timestamp Integer node can be decoded. Its units and epoch come
 * from the device or producer.
 *
 * @param[in] snapshot Owned event snapshot.
 * @param[out] info Receives the metadata and borrowed payload pointer.
 * @return IRIS_OK or IRIS_INVALID_ARGUMENT.
 */
iris_status_t iris_event_snapshot_info(const iris_event_snapshot_t *snapshot, iris_event_snapshot_info_t *info);

/**
 * @brief Copy a GenApi feature value captured for this event.
 *
 * Use an entry name from the snapshot's feature list. A feature that could
 * not be decoded reports its captured error without affecting other values.
 *
 * @param[in] snapshot Owned event snapshot.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_NOT_FOUND, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_event_snapshot_feature_to_string(const iris_event_snapshot_t *snapshot, const char *name,
                                                    char *text, size_t *size);

/**
 * @brief Destroy a snapshot and invalidate its borrowed views.
 * @param[in,out] snapshot Pointer to the owned handle. Cleared on return. NULL is allowed.
 */
void iris_event_snapshot_close(iris_event_snapshot_t **snapshot);

/** Principal GenApi interface of a feature. */
typedef int32_t iris_feature_type_t;
enum {
    IRIS_FEATURE_INTEGER = 1,
    IRIS_FEATURE_FLOAT = 2,
    IRIS_FEATURE_BOOLEAN = 3,
    IRIS_FEATURE_ENUMERATION = 4,
    IRIS_FEATURE_STRING = 5,
    IRIS_FEATURE_COMMAND = 6,
    IRIS_FEATURE_OTHER = 7,
    IRIS_FEATURE_CATEGORY = 8,
    IRIS_FEATURE_REGISTER = 9
};

/** Access of a feature or enumeration entry in the current selector state. */
typedef int32_t iris_feature_access_t;
enum {
    IRIS_FEATURE_NOT_IMPLEMENTED = 1,
    IRIS_FEATURE_NOT_AVAILABLE = 2,
    IRIS_FEATURE_WRITE_ONLY = 3,
    IRIS_FEATURE_READ_ONLY = 4,
    IRIS_FEATURE_READ_WRITE = 5
};

/** Numeric feature increment rule. */
typedef int32_t iris_increment_mode_t;
enum {
    IRIS_INCREMENT_NONE = 1,
    IRIS_INCREMENT_FIXED = 2,
    IRIS_INCREMENT_LIST = 3
};

/** Feature classification returned by iris_feature_info(). */
typedef struct {
    iris_feature_type_t type;      /**< Principal interface. */
    iris_feature_access_t access;  /**< Current access mode. */
} iris_feature_info_t;

/** GenApi visibility level for feature browsing. */
typedef int32_t iris_feature_visibility_t;
enum {
    IRIS_VISIBILITY_BEGINNER = 0,
    IRIS_VISIBILITY_EXPERT = 1,
    IRIS_VISIBILITY_GURU = 2,
    IRIS_VISIBILITY_INVISIBLE = 3
};

/** Relationship between the named node and the returned features. */
typedef int32_t iris_feature_relation_t;
enum {
    IRIS_CATEGORY_FEATURES = 1,   /**< Direct members of a category. */
    IRIS_SELECTED_FEATURES = 2,   /**< Features controlled by this selector. */
    IRIS_SELECTING_FEATURES = 3   /**< Selectors that control this feature. */
};

/** Owns an immutable snapshot of GenApi features and their metadata. */
typedef struct iris_feature_list iris_feature_list_t;

/**
 * @brief Borrow a snapshot's name-sorted GenApi feature list.
 *
 * Inspect the list with iris_feature_list_count() and iris_feature_list_get().
 * Its lifetime follows the event snapshot.
 *
 * @param[in] snapshot Owned event snapshot.
 * @return Borrowed feature list, or NULL for a NULL snapshot.
 */
const iris_feature_list_t *iris_event_snapshot_features(const iris_event_snapshot_t *snapshot);

/** Feature metadata captured when the snapshot was created. */
typedef struct {
    const char *name;             /**< Name usable with Iris feature accessors. */
    const char *display_name;     /**< Human-readable name. */
    const char *description;      /**< Description supplied by the node. */
    const char *tooltip;          /**< Short help text supplied by the node. */
    iris_feature_info_t info;     /**< Type and access at the time of the query. */
    iris_feature_visibility_t visibility; /**< GenApi visibility level. */
} iris_feature_entry_t;

/** Suggested GenApi display representation for a numeric feature. */
typedef int32_t iris_feature_representation_t;
enum {
    IRIS_REPRESENTATION_UNSPECIFIED = 0, /**< No numeric representation reported. */
    IRIS_REPRESENTATION_LINEAR = 1,      /**< Linear scale. */
    IRIS_REPRESENTATION_LOGARITHMIC = 2, /**< Logarithmic scale. */
    IRIS_REPRESENTATION_BOOLEAN = 3,     /**< Boolean control. */
    IRIS_REPRESENTATION_PURE_NUMBER = 4, /**< Decimal numeric field. */
    IRIS_REPRESENTATION_HEX_NUMBER = 5,  /**< Hexadecimal numeric field. */
    IRIS_REPRESENTATION_IPV4_ADDRESS = 6, /**< IPv4 address. */
    IRIS_REPRESENTATION_MAC_ADDRESS = 7   /**< MAC address. */
};

/** GenApi Float display notation. */
typedef int32_t iris_feature_display_notation_t;
enum {
    IRIS_DISPLAY_NOTATION_UNSPECIFIED = 0, /**< No Float notation reported. */
    IRIS_DISPLAY_NOTATION_AUTOMATIC = 1,   /**< Automatic notation. */
    IRIS_DISPLAY_NOTATION_FIXED = 2,       /**< Fixed notation. */
    IRIS_DISPLAY_NOTATION_SCIENTIFIC = 3   /**< Scientific notation. */
};

/** Metadata for one named feature in the current selector state. */
typedef struct {
    iris_feature_entry_t feature;                 /**< Feature identity, documentation, access, and visibility. */
    int64_t display_precision;                    /**< GenApi Float display precision, or -1 if unavailable. */
    iris_feature_representation_t representation; /**< GenApi Integer or Float representation. */
    iris_feature_display_notation_t display_notation; /**< GenApi Float display notation. */
} iris_feature_metadata_t;

/**
 * @brief Read metadata directly by GenApi feature name.
 *
 * Pass NULL for strings with an input size of zero to query the total size
 * for the four NUL-terminated strings. This also returns scalar metadata,
 * with the string pointers set to NULL. On a successful copy the string
 * pointers reference the caller's storage. They remain valid after the map
 * closes for as long as that storage is retained. Requery to refresh metadata
 * after selector or device state changes.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] metadata Receives metadata on success.
 * @param[out] strings Storage for the strings, or NULL for a size query.
 * @param[in,out] size Available bytes on input, required bytes on success or IRIS_BUFFER_TOO_SMALL.
 * @return IRIS_OK, IRIS_NOT_FOUND, IRIS_BUFFER_TOO_SMALL with storage and
 *         metadata untouched, or another negative status.
 */
iris_status_t iris_feature_metadata(iris_node_map_t *map, const char *name,
                                    iris_feature_metadata_t *metadata, char *strings, size_t *size);

/**
 * @brief Browse a category or inspect the selector relationships of a feature.
 *
 * Includes unavailable features. This control operation may allocate and
 * read the device. Requery access after changing a selector or device state.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Category or feature name. NULL selects Root for IRIS_CATEGORY_FEATURES.
 * @param[in] relation Category members, selected features, or selecting features.
 * @param[out] list Receives a snapshot that can outlive the node map.
 * @return IRIS_OK, IRIS_NOT_FOUND for a missing node, IRIS_WRONG_TYPE for a
 *         non-category in category mode, or another negative status.
 */
iris_status_t iris_node_map_browse(iris_node_map_t *map, const char *name,
                                  iris_feature_relation_t relation, iris_feature_list_t **list);

/** Name pattern syntax for flat feature listing. */
typedef int32_t iris_feature_match_t;
enum {
    IRIS_FEATURE_MATCH_GLOB = 0,  /**< Case-sensitive glob against the whole name. */
    IRIS_FEATURE_MATCH_REGEX = 1  /**< Case-sensitive ECMAScript regular expression search. */
};

enum {
    IRIS_FEATURE_LIST_AVAILABLE_ONLY = 1u << 0,  /**< Include only available nodes in the current selector state. */
    IRIS_FEATURE_LIST_INCLUDE_INTERNAL = 1u << 1 /**< Include nodes outside the Root category tree. */
};

/** Optional filters for a flat feature snapshot. A zero value selects all public features. */
typedef struct {
    const char *name_pattern;    /**< Pattern, or NULL to match every name. */
    iris_feature_match_t match;  /**< Syntax of name_pattern. */
    iris_feature_type_t type;    /**< Principal type, or zero to match every type. */
    uint32_t flags;              /**< IRIS_FEATURE_LIST_* bits. */
} iris_feature_filter_t;

/**
 * @brief List GenApi features as a flat snapshot sorted by name.
 *
 * Applies all supplied filters. Access is evaluated in the current selector
 * state. This control operation can allocate and read the device.
 *
 * @param[in] map Borrowed node map.
 * @param[in] filter Optional filters. NULL selects all features reachable from Root.
 * @param[out] list Receives an owned snapshot, including when no feature matches.
 * @return IRIS_OK, IRIS_INVALID_ARGUMENT for invalid filters, or another negative status.
 */
iris_status_t iris_node_map_list_features(iris_node_map_t *map, const iris_feature_filter_t *filter,
                                          iris_feature_list_t **list);

/**
 * @brief Count the entries in a feature snapshot.
 * @param[in] list Feature snapshot or NULL.
 * @return The number of entries, or zero for NULL.
 */
size_t iris_feature_list_count(const iris_feature_list_t *list);

/**
 * @brief Read a feature from a snapshot.
 * @param[in] list Feature snapshot.
 * @param[in] index Zero-based entry index.
 * @param[out] entry Feature metadata. The strings are borrowed until list close.
 * @return IRIS_OK or IRIS_INVALID_ARGUMENT.
 */
iris_status_t iris_feature_list_get(const iris_feature_list_t *list, size_t index,
                                    iris_feature_entry_t *entry);

/**
 * @brief Destroy a feature snapshot and its strings.
 * @param[in,out] list Pointer to the handle, cleared on return. NULL is allowed.
 */
void iris_feature_list_close(iris_feature_list_t **list);

/** Integer constraints in feature units. */
typedef struct {
    int64_t minimum;                       /**< Current minimum. */
    int64_t maximum;                       /**< Current maximum. */
    int64_t increment;                     /**< Fixed step, or zero in other modes. */
    iris_increment_mode_t increment_mode;  /**< Current increment rule. */
} iris_integer_range_t;

/** Float constraints in feature units. */
typedef struct {
    double minimum;                        /**< Current minimum. */
    double maximum;                        /**< Current maximum. */
    double increment;                      /**< Fixed step, or zero in other modes. */
    iris_increment_mode_t increment_mode;  /**< Current increment rule. */
} iris_float_range_t;

/**
 * @name GenApi feature access
 *
 * Text getters include the NUL in *size. Query with NULL output and *size=0.
 * On BUFFER_TOO_SMALL, *size is the required size and output is untouched.
 * Array getters follow the same convention with counts in elements.
 * Numeric list queries require the GenApi node to report listIncrement.
 * @{
 */
/**
 * @brief Read the feature's principal type and current access mode.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] info Receives the feature type and access mode.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_info(iris_node_map_t *map, const char *name, iris_feature_info_t *info);

/**
 * @brief Copy the alias target's GenApi name.
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_NOT_FOUND if the feature or alias is absent, or another negative status.
 */
iris_status_t iris_feature_alias(iris_node_map_t *map, const char *name, char *text, size_t *size);

/**
 * @brief Copy the hexadecimal event ID of a named feature.
 *
 * Uses the node's own ID when defined, or the unique ID of the event ports
 * it depends on. Returns the XML ID as text, including IDs wider than 64 bits.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_NOT_FOUND if the feature or ID is absent, IRIS_AMBIGUOUS
 *         if dependencies have different IDs, or another negative status.
 */
iris_status_t iris_feature_event_id(iris_node_map_t *map, const char *name, char *text, size_t *size);

/**
 * @brief Query the GenApi maximum length of a named String feature.
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] length Receives IString's maximum length in bytes.
 * @return IRIS_OK, IRIS_WRONG_TYPE for a non-String feature, or another negative status.
 */
iris_status_t iris_feature_string_max_length(iris_node_map_t *map, const char *name, size_t *length);

/**
 * @brief Copy the XML element with the given Name from the loaded description.
 *
 * Returns a UTF-8 serialization of the element and its children. Generated
 * GenApi nodes may have no corresponding XML element. A name shared by more
 * than one element is ambiguous.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive XML Name attribute.
 * @param[out] xml Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_NOT_FOUND, IRIS_AMBIGUOUS, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_xml(iris_node_map_t *map, const char *name, char *xml, size_t *size);

/**
 * @brief Invalidate one feature and its dependent nodes through GenApi.
 *
 * The call takes the feature lock. Later feature queries refresh the
 * invalidated information. It requires no GenTL event subscription.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @return IRIS_OK, IRIS_NOT_FOUND, or another negative status.
 */
iris_status_t iris_feature_invalidate(iris_node_map_t *map, const char *name);
/**
 * @brief Read a feature using GenApi IValue text conversion.
 *
 * Use for textual configuration values. Read a String feature directly with
 * iris_feature_get_string().
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination for NUL-terminated text, or NULL to query size.
 * @param[in,out] size Capacity in bytes on input. Receives the required size including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_to_string(iris_node_map_t *map, const char *name,
                                     char *text, size_t *size);
/**
 * @brief Write a feature using GenApi IValue text conversion.
 *
 * GenApi parses and validates the text. Use iris_feature_execute() for Commands.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value NUL-terminated value to write.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_from_string(iris_node_map_t *map, const char *name, const char *value);

/** One named GenApi write using the same text format as iris_feature_from_string(). */
typedef struct {
    const char *name;  /**< Nonempty, case-sensitive GenApi feature name. */
    const char *value; /**< NUL-terminated value to write. */
} iris_feature_write_t;

/**
 * @brief Apply feature writes in array order under one feature lock.
 *
 * Each write uses the state left by the previous writes. Other Iris feature
 * calls cannot interleave. Serialize with stream start, stop, and owner close
 * as for individual feature calls. This control operation can block and allocate.
 * Commands require a separate iris_feature_execute() call.
 *
 * Stops at the first error. Earlier writes remain applied. The failed write
 * may have affected the device. The operation provides no rollback or
 * device-side atomic application. Selectors stay at their last written values.
 *
 * @param[in] map Borrowed node map.
 * @param[in] writes Array of count writes. May be NULL when count is zero.
 * @param[in] count Number of writes. Zero succeeds without writing.
 * @param[out] applied Number of writes that returned success. On a write error,
 *                     this is the zero-based index of the failing entry.
 * @return IRIS_OK if all writes succeeded, or the first negative status.
 */
iris_status_t iris_feature_write_batch(iris_node_map_t *map, const iris_feature_write_t *writes,
                                       size_t count, size_t *applied);

/**
 * @brief Start staging GenApi writes for a native GenTL port stack on one map.
 *
 * Stage writes with iris_node_map_stack_add(). A second start on the same map
 * returns IRIS_BUSY. Serialize the sequence if several callers share the map.
 * The map owns the stack and discards pending entries at owner close.
 *
 * @param[in] map Borrowed node map.
 * @return IRIS_OK, IRIS_BUSY, IRIS_NOT_SUPPORTED for a missing stacked-write
 *         entry point, or another negative status.
 */
iris_status_t iris_node_map_start_stacking(iris_node_map_t *map);

/**
 * @brief Add a named GenApi text write to the active stack.
 *
 * Copies the name and value and checks the node's interface. Access, selector,
 * and value validation take place when the stack is written. Use
 * iris_feature_execute() separately for GenApi Commands.
 *
 * @param[in] map Borrowed node map with an active stack.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value NUL-terminated value in GenApi text format.
 * @return IRIS_OK, IRIS_INVALID_STATE for an idle stack, or another negative status.
 */
iris_status_t iris_node_map_stack_add(iris_node_map_t *map, const char *name, const char *value);

/**
 * @brief Apply staged writes through GenApi and GCWritePortStacked.
 *
 * Consumes the stack on any execution result. An empty stack reports zero
 * writes and succeeds. GenApi can issue multiple
 * native calls when a later feature requires a register read. Earlier or
 * later entries can take effect when GenApi reports a feature error. The
 * operation has no device-side atomicity or rollback. Cache entries are
 * invalidated after execution so subsequent queries refresh them.
 * This control call takes the feature lock, can allocate, and must be
 * serialized with acquisition start and stop on the associated device.
 *
 * @param[in] map Borrowed node map with an active stack.
 * @param[out] registers_written Number of register writes confirmed by the
 *                               producer, including on a partial failure.
 *                               A feature can write multiple registers.
 * @return IRIS_OK, IRIS_INVALID_STATE for an idle stack, IRIS_NOT_SUPPORTED if
 *         the producer rejects stacked writes, or another negative status.
 */
iris_status_t iris_node_map_write_stack(iris_node_map_t *map, size_t *registers_written);

/**
 * @brief Discard staged writes without applying them.
 * @param[in] map Borrowed node map. Repeating cancel on an idle map succeeds.
 * @return IRIS_OK or a negative status.
 */
iris_status_t iris_node_map_cancel_stack(iris_node_map_t *map);

/** Current GenApi register extent. Selectors can change either field. */
typedef struct {
    uint64_t address; /**< Byte address within the register's GenApi port. */
    size_t length;    /**< Register length in bytes. */
} iris_register_info_t;

/**
 * @brief Query the current address and length of a named GenApi IRegister.
 *
 * A feature whose principal type is Integer or String may also implement
 * IRegister and support this operation.
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive feature name.
 * @param[out] info Receives the register extent.
 * @return IRIS_OK, IRIS_WRONG_TYPE for a feature without IRegister, or another negative status.
 */
iris_status_t iris_feature_register_info(iris_node_map_t *map, const char *name,
                                         iris_register_info_t *info);
/**
 * @brief Read a complete named GenApi register into caller storage.
 *
 * Returns raw bytes in port order and bypasses the GenApi value cache.
 * Query size with NULL data and zero input size. If the buffer is too small,
 * the function returns the required size without writing to it.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive feature name.
 * @param[out] data Destination buffer, or NULL to query size.
 * @param[in,out] size Destination capacity and required length in bytes.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_read_register(iris_node_map_t *map, const char *name, void *data,
                                         size_t *size);
/**
 * @brief Write a complete named GenApi register from caller storage.
 *
 * Uses GenApi access checks and dependency invalidation. Pass raw bytes in
 * port order. The current selector state determines the address and length.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive feature name.
 * @param[in] data Source bytes.
 * @param[in] size Exact current register length in bytes.
 * @return IRIS_OK, IRIS_OUT_OF_RANGE for a length mismatch, or another negative status.
 */
iris_status_t iris_feature_write_register(iris_node_map_t *map, const char *name, const void *data,
                                          size_t size);

/**
 * @brief Read the unit string of an Integer or Float feature.
 *
 * A node with no unit returns an empty string.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination for NUL-terminated text, or NULL to query size.
 * @param[in,out] size Capacity in bytes on input. Receives the required size including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, IRIS_WRONG_TYPE for nonnumeric features, or another negative status.
 */
iris_status_t iris_feature_unit(iris_node_map_t *map, const char *name, char *text, size_t *size);
/**
 * @brief Read the current value of an Integer feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives the value in feature units.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_get_integer(iris_node_map_t *map, const char *name, int64_t *value);
/**
 * @brief Write a value to an Integer feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value New value in feature units. GenApi validates it against the node constraints.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_set_integer(iris_node_map_t *map, const char *name, int64_t value);
/**
 * @brief Read the current limits and increment mode of an Integer feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] range Receives minimum, maximum, increment mode, and fixed increment. Increment is zero in other modes.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_integer_range(iris_node_map_t *map, const char *name,
                                         iris_integer_range_t *range);
/**
 * @brief Read the discrete allowed values of an Integer feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] values Destination array, or NULL to query count.
 * @param[in,out] count Capacity in elements on input. Receives the required count. Set to zero for a count query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, IRIS_NOT_SUPPORTED without list increment mode, or another negative status.
 */
iris_status_t iris_feature_integer_values(iris_node_map_t *map, const char *name,
                                          int64_t *values, size_t *count);
/**
 * @brief Read the current value of a Float feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives the value in feature units.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_get_float(iris_node_map_t *map, const char *name, double *value);
/**
 * @brief Write a value to a Float feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value New value in feature units. GenApi validates it against the node constraints.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_set_float(iris_node_map_t *map, const char *name, double value);
/**
 * @brief Read the current limits and increment mode of a Float feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] range Receives minimum, maximum, increment mode, and fixed increment. Increment is zero in other modes.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_float_range(iris_node_map_t *map, const char *name,
                                       iris_float_range_t *range);
/**
 * @brief Read the discrete allowed values of a Float feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] values Destination array, or NULL to query count.
 * @param[in,out] count Capacity in elements on input. Receives the required count. Set to zero for a count query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, IRIS_NOT_SUPPORTED without list increment mode, or another negative status.
 */
iris_status_t iris_feature_float_values(iris_node_map_t *map, const char *name,
                                        double *values, size_t *count);
/**
 * @brief Read a Boolean feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives 0 for false or 1 for true.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_get_boolean(iris_node_map_t *map, const char *name, uint32_t *value);
/**
 * @brief Write a Boolean feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value 0 for false or 1 for true.
 * @return IRIS_OK, IRIS_INVALID_ARGUMENT for values other than 0 or 1, or another negative status.
 */
iris_status_t iris_feature_set_boolean(iris_node_map_t *map, const char *name, uint32_t value);
/**
 * @brief Read the contents of a GenApi String feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination for NUL-terminated text, or NULL to query size.
 * @param[in,out] size Capacity in bytes on input. Receives the required size including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_get_string(iris_node_map_t *map, const char *name,
                                      char *text, size_t *size);
/**
 * @brief Write the contents of a GenApi String feature.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] value NUL-terminated value to write.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_set_string(iris_node_map_t *map, const char *name, const char *value);
/**
 * @brief Read the symbolic name of the selected enumeration entry.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination for NUL-terminated text, or NULL to query size.
 * @param[in,out] size Capacity in bytes on input. Receives the required size including NUL. Set to zero for a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_get_enumeration(iris_node_map_t *map, const char *name,
                                           char *text, size_t *size);
/**
 * @brief Select an enumeration entry by symbolic name.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] symbol Case-sensitive NUL-terminated entry name.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_set_enumeration(iris_node_map_t *map, const char *name,
                                           const char *symbol);
/**
 * @brief Count all entries of an Enumeration feature, including unavailable entries.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] count Receives the number of entries.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_enumeration_count(iris_node_map_t *map, const char *name, size_t *count);
/**
 * @brief Read an enumeration entry's symbolic name and access mode.
 *
 * Unavailable entries are included. Use iris_feature_enumeration_count() to
 * obtain the entry count.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[in] index Zero-based entry index for the current selector state.
 * @param[out] text Destination for NUL-terminated text, or NULL to query size.
 * @param[in,out] size Capacity in bytes on input. Receives the required size including NUL. Set to zero for a size query.
 * @param[out] access Receives the current entry access, including during a size query.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_feature_enumeration_entry(iris_node_map_t *map, const char *name,
                                             size_t index, char *text, size_t *size,
                                             iris_feature_access_t *access);
/**
 * @brief Execute a GenApi Command feature.
 *
 * Use iris_feature_command_done() to query completion when the device
 * executes the command asynchronously.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @return IRIS_OK when the command is issued, or a negative status.
 */
iris_status_t iris_feature_execute(iris_node_map_t *map, const char *name);
/**
 * @brief Query whether execution of a GenApi Command has completed.
 *
 * @param[in] map Borrowed node map.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] done Receives 1 if done or 0 if still pending.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_feature_command_done(iris_node_map_t *map, const char *name, uint32_t *done);
/** @} */

/** Determines whether Iris issues the remote AcquisitionStart/Stop commands. */
typedef int32_t iris_acquisition_control_t;
enum {
    IRIS_ACQUISITION_REMOTE_AND_STREAM = 1,
    IRIS_ACQUISITION_STREAM_ONLY = 2
};

/** Application-owned host memory for one announced GenTL buffer. */
typedef struct {
    void *data;       /**< Writable host memory satisfying stream alignment. */
    size_t size;      /**< Capacity in bytes. */
    void *user_data;  /**< Optional application pointer returned with completions. */
} iris_buffer_t;

/** Requirements for the stream under its current acquisition settings. */
typedef struct {
    size_t payload_size;          /**< Minimum bytes per buffer. */
    size_t minimum_buffer_count;  /**< Minimum number of announced buffers. */
    size_t alignment;             /**< Required byte alignment, or 1 if unreported. */
} iris_buffer_requirements_t;

/**
 * @brief Query buffer requirements after configuring the device.
 * @param[in] stream Open, idle stream.
 * @param[out] requirements Receives payload size, minimum count, and alignment.
 * @return IRIS_OK, IRIS_BUSY during acquisition or cleanup, or a negative status.
 */
iris_status_t iris_stream_buffer_requirements(iris_stream_t *stream,
                                               iris_buffer_requirements_t *requirements);

/** Stream startup settings. A NULL options pointer selects library-managed buffers. */
typedef struct {
    uint32_t buffer_count;                          /**< Zero selects the default count for library-managed buffers. */
    iris_acquisition_control_t acquisition_control; /**< Host-only or host and remote control. */
    const iris_buffer_t *buffers;                    /**< Optional array of buffer_count caller buffers. */
} iris_start_options_t;

/**
 * @brief Announce and queue acquisition buffers and start a stream.
 *
 * The default uses at least eight buffers and respects the producer minimum.
 * An explicit count below that minimum returns IRIS_OUT_OF_RANGE. Only one
 * stream can acquire per device. By default Iris controls remote acquisition
 * and the host stream. In stream-only mode the caller controls the remote device.
 * For a fixed-size producer pool, pass its required count explicitly.
 * With caller-owned buffers, supply a positive count and nonoverlapping
 * ranges that meet the current requirements. Each range must be exclusive to
 * this pool. The descriptor array is read only during start. Keep the memory
 * valid and untouched outside frame borrows until a successful stop performed
 * after all borrows are released, or until successful close. If start fails,
 * call stop and check that it succeeds before freeing or reusing the memory.
 * Some producers require their own allocation or require reopening a stream
 * before changing allocation mode.
 *
 * @param[in] stream Open stream.
 * @param[in] options Buffer count, acquisition control, and optional caller buffers, or NULL for defaults.
 * @return IRIS_OK, IRIS_BUSY for active acquisition or pending cleanup, or another negative status.
 */
iris_status_t iris_stream_start(iris_stream_t *stream, const iris_start_options_t *options);
/**
 * @brief Stop acquisition and wake a blocked reader.
 *
 * Stop may run concurrently with receive and release on this stream.
 * Outstanding frame borrows remain valid. Release them before restarting or
 * closing. After the final release, restart or close retires the old buffers.
 * Iris clears the transport parameter lock it acquired once it revokes the old buffers.
 * Retry stop if cleanup reports an error.
 *
 * @param[in] stream Open stream.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_stream_stop(iris_stream_t *stream);

/** Classification of a completed buffer's image. */
typedef int32_t iris_frame_status_t;
enum {
    IRIS_FRAME_COMPLETE = 1,
    IRIS_FRAME_INCOMPLETE = 2,
    IRIS_FRAME_UNSUPPORTED_PAYLOAD = 3,
    IRIS_FRAME_INVALID_IMAGE = 4
};

enum {
    IRIS_FRAME_HAS_STRIDE = 1u << 0,
    IRIS_FRAME_HAS_PIXEL_FORMAT = 1u << 1,
    IRIS_FRAME_HAS_ID = 1u << 2,
    IRIS_FRAME_HAS_TIMESTAMP_NS = 1u << 3,
    IRIS_FRAME_HAS_HOST_TIMESTAMP_NS = 1u << 4,
    IRIS_FRAME_HAS_PIXEL_ENDIANNESS = 1u << 5
};

/** GenTL payload type. Producers can return additional numeric codes. */
typedef uint64_t iris_payload_type_t;
enum {
    IRIS_PAYLOAD_UNKNOWN = 0,
    IRIS_PAYLOAD_IMAGE = 1,
    IRIS_PAYLOAD_RAW_DATA = 2,
    IRIS_PAYLOAD_FILE = 3,
    IRIS_PAYLOAD_CHUNK_DATA = 4,
    IRIS_PAYLOAD_JPEG = 5,
    IRIS_PAYLOAD_JPEG2000 = 6,
    IRIS_PAYLOAD_H264 = 7,
    IRIS_PAYLOAD_CHUNK_ONLY = 8,
    IRIS_PAYLOAD_DEVICE_SPECIFIC = 9,
    IRIS_PAYLOAD_MULTI_PART = 10,
    IRIS_PAYLOAD_GENDC = 11
};

enum {
    IRIS_PAYLOAD_HAS_TYPE = 1u << 0,
    IRIS_PAYLOAD_HAS_SIZE_FILLED = 1u << 1,
    IRIS_PAYLOAD_HAS_DATA_SIZE = 1u << 2,
    IRIS_PAYLOAD_HAS_IMAGE_OFFSET = 1u << 3,
    IRIS_PAYLOAD_HAS_DELIVERED_IMAGE_HEIGHT = 1u << 4,
    IRIS_PAYLOAD_HAS_INCOMPLETE = 1u << 5,
    IRIS_PAYLOAD_HAS_CHUNK_DATA = 1u << 6,
    IRIS_PAYLOAD_HAS_CHUNK_LAYOUT_ID = 1u << 7,
    IRIS_PAYLOAD_IS_COMPOSITE = 1u << 8 /**< Producer reported discontiguous storage. */
};

/** Completion metadata. Optional fields are identified by valid. When data is
 * available, the bounded raw view lasts until frame release and may contain
 * transport padding or partial data. Use the part views for multipart and
 * GenDC payloads, where size_filled counts bytes written across the buffer. */
typedef struct {
    const void *data;                   /**< Raw byte view, or NULL for structured, composite, or unbounded payloads. */
    size_t capacity;                    /**< Buffer capacity passed at announcement. */
    size_t size_filled;                 /**< Producer-reported bytes filled. */
    size_t data_size;                   /**< Producer-reported BUFFER_INFO_DATA_SIZE. */
    size_t image_offset;                /**< Producer-reported image offset in bytes. */
    size_t delivered_image_height;      /**< Producer-reported delivered image rows. */
    uint64_t chunk_layout_id;           /**< Producer-reported chunk layout identifier. */
    iris_payload_type_t type;           /**< Producer-reported GenTL payload type. */
    uint32_t valid;                     /**< IRIS_PAYLOAD_HAS_* bits. */
    uint32_t incomplete;                /**< Producer-reported incomplete flag. */
    uint32_t contains_chunk_data;       /**< Producer-reported chunk-data flag. */
} iris_payload_t;

enum {
    IRIS_PART_HAS_FORMAT = 1u << 0,
    IRIS_PART_HAS_WIDTH = 1u << 1,
    IRIS_PART_HAS_HEIGHT = 1u << 2,
    IRIS_PART_HAS_SOURCE_ID = 1u << 3,
    IRIS_PART_HAS_REGION_ID = 1u << 4,
    IRIS_PART_HAS_COMPONENT_TYPE = 1u << 5,
    IRIS_PART_HAS_X_PADDING = 1u << 6,
    IRIS_PART_HAS_DELIVERED_HEIGHT = 1u << 7
};

/** Maximum number of parts that Iris stores per completion. */
#define IRIS_MAX_FRAME_PARTS 32

/** Bounded zero-copy view of one multipart or contiguous GenDC data part. */
typedef struct {
    const void *data;           /**< Part bytes with the same lifetime as the frame. */
    size_t size;                /**< Size in bytes. */
    uint64_t type;              /**< GenTL PARTDATATYPE_ID or GenDC part header type. */
    uint64_t format;            /**< Data format code. */
    uint64_t format_namespace;  /**< GenTL pixel format namespace. */
    size_t width;               /**< Part width in pixels. */
    size_t height;              /**< Nominal part height in pixels. */
    size_t delivered_height;    /**< Delivered part height in pixels. */
    size_t x_padding;           /**< Row padding in bytes. */
    uint64_t source_id;         /**< Source identifier. */
    uint64_t region_id;         /**< Region identifier. */
    uint64_t component_type;    /**< GenDC component type identifier. */
    uint32_t valid;             /**< IRIS_PART_HAS_* bits. */
} iris_part_t;

/** COMPLETE exposes one verified image. Supported row layouts are Mono8/8s,
 * unpacked Mono10/12/14/16, Bayer8, RGB8/BGR8/RGBa8/BGRa8, and Mono10p/12p/14p
 * with whole packing groups per row. Other layouts report UNSUPPORTED_PAYLOAD.
 * Completions with other statuses may carry metadata. Their data is NULL and
 * image_size is zero. Consult valid before using optional fields.
 * Pixel format and namespace are GenTL
 * BUFFER_INFO_PIXELFORMAT and BUFFER_INFO_PIXELFORMAT_NAMESPACE values.
 * pixel_endianness uses GenTL PIXELENDIANNESS_ID values.
 * timestamp_ns is producer-reported, with producer-defined clock/epoch.
 * host_timestamp_ns is CLOCK_MONOTONIC at event retrieval by this process.
 * The token belongs to the issuing stream and is opaque to callers. */
typedef struct {
    const void *data;              /**< Borrowed pixel storage for a complete image. */
    size_t image_size;             /**< Image bytes, including row padding. */
    size_t stride_bytes;           /**< Byte offset between successive rows. */
    uint64_t width;                /**< Image width in pixels. */
    uint64_t height;               /**< Image height in rows. */
    uint64_t pixel_format;         /**< Producer-reported pixel format code. */
    void *user_data;                /**< Application pointer from the announced buffer, or NULL. */
    iris_frame_status_t status;     /**< Image classification for this completion. */
    uint32_t valid;                 /**< IRIS_FRAME_HAS_* bits for optional metadata. */
    uint64_t frame_id;             /**< Producer-reported frame ID. */
    uint64_t timestamp_ns;         /**< Producer timestamp in nanoseconds. */
    uint64_t host_timestamp_ns;    /**< Host monotonic timestamp in nanoseconds. */
    uint64_t token;                /**< Opaque stream-specific borrow token. */
    uint32_t pixel_format_namespace; /**< Namespace for pixel_format. */
    int32_t pixel_endianness;       /**< GenTL byte-order code. */
    const iris_part_t *parts;        /**< Borrowed part array, including during a poll handler. */
    size_t part_count;               /**< Number of entries in parts. */
    iris_payload_t payload;          /**< Raw buffer metadata for this completion. */
    iris_status_t parts_status;      /**< IRIS_OK or the error reading the part array. */
} iris_frame_t;

/**
 * @name Frame chunk features
 *
 * Use a frame borrowed through iris_stream_try_get_frame() or
 * iris_stream_wait_frame(). Chunk operations use the remote XML and the
 * borrowed frame's chunk descriptors. They take control locks and can
 * allocate. Call them outside poll handlers and before releasing the frame.
 * Typed reads require the corresponding GenApi interface. A missing or
 * unreadable chunk returns IRIS_NOT_FOUND. A non-chunk feature or wrong
 * interface returns IRIS_WRONG_TYPE.
 *
 * For text and byte getters, query required size with NULL output and
 * zero input size. IRIS_BUFFER_TOO_SMALL leaves the destination untouched.
 * @{
 */
/**
 * @brief List the readable GenApi chunk features present in a frame.
 *
 * Returns a name-sorted metadata snapshot. The snapshot can outlive the
 * frame and stream. Values are read separately while the frame is borrowed.
 *
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[out] list Receives the snapshot. Close it with iris_feature_list_close().
 * @return IRIS_OK, IRIS_NOT_SUPPORTED for an unsupported chunk layout or no
 *         chunk payload, or another negative status.
 */
iris_status_t iris_frame_chunk_list(iris_stream_t *stream, const iris_frame_t *frame,
                                    iris_feature_list_t **list);

/**
 * @brief Decode a named GenApi chunk feature from an explicitly borrowed frame.
 *
 * Uses the remote XML with GenTL chunk descriptors or a single contiguous
 * GenDC chunk metadata part without a chunk-marked embedded XML part. Call while
 * the frame remains borrowed, before releasing it or closing the stream.
 * This control operation can allocate. Call it outside poll handlers.
 * Text size includes NUL. Pass NULL and size zero for a size query.
 *
 * @param[in] stream Issuing stream.
 * @param[in] frame Live frame received through try or wait.
 * @param[in] name Case-sensitive chunk feature name from the device XML.
 * @param[out] text Destination for the GenApi text representation, or NULL.
 * @param[in,out] size Output capacity and required bytes.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, IRIS_NOT_SUPPORTED for an unsupported
 *         chunk layout, or another negative status.
 */
iris_status_t iris_frame_chunk_to_string(iris_stream_t *stream, const iris_frame_t *frame,
                                        const char *name, char *text, size_t *size);

/**
 * @brief Read an Integer chunk feature.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives the integer in the feature's units.
 * @return IRIS_OK or a negative status.
 */
iris_status_t iris_frame_chunk_get_integer(iris_stream_t *stream, const iris_frame_t *frame,
                                           const char *name, int64_t *value);
/**
 * @brief Read a Float chunk feature.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives the value in the feature's units.
 * @return IRIS_OK or a negative status.
 */
iris_status_t iris_frame_chunk_get_float(iris_stream_t *stream, const iris_frame_t *frame,
                                         const char *name, double *value);
/**
 * @brief Read a Boolean chunk feature.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] value Receives zero for false or one for true.
 * @return IRIS_OK or a negative status.
 */
iris_status_t iris_frame_chunk_get_boolean(iris_stream_t *stream, const iris_frame_t *frame,
                                           const char *name, uint32_t *value);
/**
 * @brief Copy the value of a String chunk feature.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_frame_chunk_get_string(iris_stream_t *stream, const iris_frame_t *frame,
                                          const char *name, char *text, size_t *size);
/**
 * @brief Copy the symbolic value of an Enumeration chunk feature.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] text Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes including NUL.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_frame_chunk_get_enumeration(iris_stream_t *stream, const iris_frame_t *frame,
                                               const char *name, char *text, size_t *size);
/**
 * @brief Copy a complete chunk register in port byte order.
 * @param[in] stream Issuing stream.
 * @param[in] frame Live borrowed frame.
 * @param[in] name Case-sensitive GenApi feature name.
 * @param[out] data Destination, or NULL for a size query.
 * @param[in,out] size Capacity and required bytes.
 * @return IRIS_OK, IRIS_BUFFER_TOO_SMALL, or another negative status.
 */
iris_status_t iris_frame_chunk_read_register(iris_stream_t *stream, const iris_frame_t *frame,
                                             const char *name, void *data, size_t *size);
/** @} */

/**
 * @name Frame reception
 *
 * Use one reader per stream across try, wait and poll. One thread may service
 * several streams with try or bounded poll, or each stream may have its
 * own reader.
 *
 * From the first receive after successful start, the consumer makes zero heap
 * allocations during try, wait, poll and release in normal acquisition,
 * including empty queues, timeouts and incomplete or rejected images.
 * The consumer lends image storage without a per-frame copy. Qualify producer
 * allocations separately. Callback code owns its own allocation behavior.
 *
 * An admitted try or poll holds the stream lock through its
 * producer calls and inline callbacks. Producer calls can have their own
 * synchronization and latency.
 * @{
 */
/**
 * @brief Try to dequeue one completed buffer without waiting for a completion.
 *
 * Uses a zero GenTL event timeout and does not wait for a consumer lock.
 * Release every IRIS_OK frame, including incomplete or rejected images. Other
 * returns clear the descriptor.
 *
 * @param[in] stream Open stream.
 * @param[out] frame Receives the borrowed completion. Supply a descriptor with no live borrow.
 * @return IRIS_OK with a frame, IRIS_NO_FRAME for an empty queue, IRIS_STOPPED, IRIS_BUSY, or another negative status.
 */
iris_status_t iris_stream_try_get_frame(iris_stream_t *stream, iris_frame_t *frame);
/**
 * @brief Wait for one completed buffer.
 *
 * Stop can cancel the wait. Release every IRIS_OK frame, including incomplete
 * or rejected images. Other returns clear the descriptor.
 *
 * @param[in] stream Open stream.
 * @param[in] timeout_ms Positive finite GenTL event timeout in milliseconds.
 * @param[out] frame Receives the borrowed completion. Supply a descriptor with no live borrow.
 * @return IRIS_OK with a frame, IRIS_TIMEOUT, IRIS_STOPPED, IRIS_BUSY, or another negative status.
 */
iris_status_t iris_stream_wait_frame(iris_stream_t *stream, uint64_t timeout_ms,
                                     iris_frame_t *frame);
/**
 * @brief Return a borrowed completion to the issuing stream.
 *
 * While running, the buffer is requeued. After stop, it is retired. A valid
 * token is consumed even when requeue fails. Call stop to recover the stream.
 * An invalid token leaves ownership unchanged.
 *
 * @param[in] stream Stream that issued the frame.
 * @param[in,out] frame Frame with a live token from this stream. Cleared when consumed.
 * @return IRIS_OK on success, IRIS_INVALID_ARGUMENT for an invalid token, or another negative status.
 */
iris_status_t iris_stream_release_frame(iris_stream_t *stream, iris_frame_t *frame);

/** Inline frame handler for iris_stream_poll().
 * @param[in] client_data Application pointer supplied to poll.
 * @param[in] frame Read-only completion and image view, borrowed for this call.
 */
typedef void (*iris_frame_handler_t)(void *client_data, const iris_frame_t *frame);
/**
 * @brief Poll for ready completions and deliver them to an inline callback.
 *
 * Uses a zero GenTL event timeout and does not wait for a consumer lock.
 * Each frame is released after its callback. The callback must return normally
 * and must not call Iris. The frame and its image data are borrowed only
 * for the duration of the callback.
 *
 * @param[in] stream Open stream.
 * @param[in] handler Callback invoked once per completion.
 * @param[in] client_data Application pointer passed unchanged to the callback. May be NULL.
 * @param[in] frame_limit Positive maximum number of completions, including rejected images.
 * @param[out] delivered Number of callbacks already invoked, even if a later operation fails.
 * @return IRIS_OK with zero deliveries for an empty queue, IRIS_STOPPED, IRIS_BUSY, or another negative status.
 */
iris_status_t iris_stream_poll(iris_stream_t *stream, iris_frame_handler_t handler,
                               void *client_data, size_t frame_limit, size_t *delivered);
/** @} */

/** Consumer counters over the life of a stream. */
typedef struct {
    uint64_t delivered_frames;      /**< Delivered completions of all classifications. */
    uint64_t incomplete_frames;     /**< Completions classified as incomplete. */
    uint64_t unsupported_payloads;  /**< Completions with unsupported payloads. */
    uint64_t invalid_images;        /**< Completions classified as invalid images. */
    uint64_t outstanding_frames;   /**< Completions currently borrowed. */
    uint64_t requeue_failures;       /**< Failed attempts to return a buffer to the queue. */
} iris_stream_stats_t;

/**
 * @brief Read acquisition counters for a stream.
 *
 * Safe during acquisition. Counters are cumulative across starts.
 * outstanding_frames is the current number of borrowed frames.
 *
 * @param[in] stream Open stream.
 * @param[out] stats Receives the counter snapshot.
 * @return IRIS_OK on success or a negative status.
 */
iris_status_t iris_stream_stats(iris_stream_t *stream, iris_stream_stats_t *stats);

#ifdef __cplusplus
}
#endif
#endif
