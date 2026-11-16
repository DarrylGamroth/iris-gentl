#ifndef IRIS_GENTL_FEATURES_HPP
#define IRIS_GENTL_FEATURES_HPP

#include <iris.h>

#include <string>
#include <vector>

namespace gentl {

bool report(iris_status_t status, const char *operation);
void check(iris_status_t status, const char *operation);

std::string read_feature(iris_node_map_t *map, const char *name);
void describe(iris_node_map_t *map, const std::string &name, bool json);
void snapshot(iris_node_map_t *map, const std::string &module, const std::vector<std::string> &names, bool json);
bool diff_snapshots(const std::string &before, const std::string &after, bool json);

} // namespace gentl

#endif
