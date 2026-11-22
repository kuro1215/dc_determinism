#include <string>

#include "common/sha256.hpp"
#include "testing.hpp"

using dc_determinism::Sha256;

TEST(known_vectors) {
    CHECK_EQ(dc_determinism::sha256_hex(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
    CHECK_EQ(dc_determinism::sha256_hex("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    CHECK_EQ(dc_determinism::sha256_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
             std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
}

TEST(million_a) {
    Sha256 hash;
    std::string block(1000, 'a');
    for (int i = 0; i < 1000; ++i) hash.update(block);
    CHECK_EQ(hash.finish_hex(), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

TEST(split_updates_and_reuse) {
    Sha256 hash;
    std::string data(1000, 'a');
    hash.update(data.data(), 100);
    hash.update(data.data() + 100, 900);
    CHECK_EQ(hash.finish_hex(), std::string("41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3"));
    // finish() resets the object
    hash.update("abc", 3);
    CHECK_EQ(hash.finish_hex(), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
}

TEST(padding_boundaries) {
    // lengths 55, 56, 63, 64, 65 (padding edge cases), reference values from hashlib
    CHECK_EQ(dc_determinism::sha256_hex(std::string(55, 'a')),
             std::string("9f4390f8d30c2dd92ec9f095b65e2b9ae9b0a925a5258e241c9f1e910f734318"));
    CHECK_EQ(dc_determinism::sha256_hex(std::string(56, 'a')),
             std::string("b35439a4ac6f0948b6d6f9e3c6af0f5f590ce20f1bde7090ef7970686ec6738a"));
    CHECK_EQ(dc_determinism::sha256_hex(std::string(63, 'a')),
             std::string("7d3e74a05d7db15bce4ad9ec0658ea98e3f06eeecf16b4c6fff2da457ddc2f34"));
    CHECK_EQ(dc_determinism::sha256_hex(std::string(64, 'a')),
             std::string("ffe054fe7ae0cb6dc65c3af9b61d5209f439851db43d0ba5997337df154668eb"));
    CHECK_EQ(dc_determinism::sha256_hex(std::string(65, 'a')),
             std::string("635361c48bb9eab14198e76ea8ab7f1a41685d6ad62aa9146d301d4f17eb0ae0"));
}

int main() { return testing::run_all_tests(); }
