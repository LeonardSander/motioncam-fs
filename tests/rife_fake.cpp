#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

int main() {
    std::cout << "MOTIONCAM_READY" << std::endl;
    std::string line;
    while (std::getline(std::cin, line)) {
        std::vector<std::string> fields;
        std::stringstream row(line);
        for (std::string field; std::getline(row, field, '\t');) fields.push_back(field);
        if (fields.size() < 5) return 2;
        std::ifstream leftStream(fields[2], std::ios::binary), rightStream(fields[3], std::ios::binary);
        const std::vector<uint8_t> left{std::istreambuf_iterator<char>(leftStream), {}};
        const std::vector<uint8_t> right{std::istreambuf_iterator<char>(rightStream), {}};
        if (left.size() != right.size() || left.size() % 2) return 3;
        for (size_t job = 4; job < fields.size(); ++job) {
            const auto separator = fields[job].find('|');
            if (separator == std::string::npos) return 4;
            const double ratio = std::stod(fields[job].substr(0, separator));
            std::ofstream output(fields[job].substr(separator + 1), std::ios::binary);
            for (size_t i = 0; i < left.size(); i += 2) {
                const uint16_t a = static_cast<uint16_t>(left[i] | left[i + 1] << 8);
                const uint16_t b = static_cast<uint16_t>(right[i] | right[i + 1] << 8);
                const uint16_t value = static_cast<uint16_t>(std::lround(a * (1.0 - ratio) + b * ratio));
                output.put(static_cast<char>(value & 0xff));
                output.put(static_cast<char>(value >> 8));
            }
        }
        std::cout << "MOTIONCAM_DONE" << std::endl;
    }
}
