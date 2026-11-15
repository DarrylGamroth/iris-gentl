#include "feature_values.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#define EXPECT(x) do { if (!(x)) { std::fprintf(stderr, "%d: %s\n", __LINE__, #x); std::exit(1); } } while (0)

template<class T>
struct NumericNode {
    std::array<T, 3> values;
    GenApi::EIncMode GetIncMode() const { return GenApi::listIncrement; }
    const auto &GetListOfValidValues() const { return values; }
};

template<class T>
void test_list(const NumericNode<T> &node)
{
    std::array<T, 3> output{};
    size_t count = 0;
    EXPECT(iris::feature_values(node, static_cast<T *>(nullptr), &count, "values") == IRIS_OK);
    EXPECT(count == output.size());
    count = 2;
    EXPECT(iris::feature_values(node, output.data(), &count, "values") == IRIS_BUFFER_TOO_SMALL);
    EXPECT(count == output.size());
    EXPECT((output == std::array<T, 3>{}));
    EXPECT(iris::feature_values(node, output.data(), &count, "values") == IRIS_OK);
    EXPECT(count == node.values.size() && output == node.values);
}

int main()
{
    test_list(NumericNode<int64_t>{{16, 48, 112}});
    test_list(NumericNode<double>{{0.125, 0.75, 3.5}});
}
