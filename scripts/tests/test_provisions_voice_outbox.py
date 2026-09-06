"""Run the actual journal against real AES-GCM and a fault-injected flash image."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class VoiceOutboxTests(unittest.TestCase):
    def test_restart_torn_writes_scope_receipts_and_storage_exhaustion(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        crypto_prefix = Path("/opt/homebrew/opt/openssl@3")
        include = ["-I", str(crypto_prefix / "include")] if crypto_prefix.exists() else []
        libraries = ["-L", str(crypto_prefix / "lib")] if crypto_prefix.exists() else []
        program = r'''
#include "provisions_voice_outbox.h"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
using namespace provisions;

struct Flash : VoiceFlash {
    std::vector<uint8_t> data = std::vector<uint8_t>(VoiceOutbox::kStoreBytes, 255);
    int writes = 0, erases = 0, cut_write = -1;
    size_t cut_bytes = 0;
    bool fail_read = false, fail_erase = false, lie_write = false;
    bool Read(size_t offset, void* output, size_t bytes) override {
        assert(offset + bytes <= data.size());
        if (fail_read) return false;
        memcpy(output, data.data() + offset, bytes); return true;
    }
    bool Write(size_t offset, const void* input, size_t bytes) override {
        assert(offset + bytes <= data.size());
        const bool cut = writes++ == cut_write;
        const size_t count = cut ? std::min(bytes, cut_bytes) : bytes;
        if (!lie_write) for (size_t n = 0; n < count; ++n) {
            auto value = static_cast<const uint8_t*>(input)[n];
            assert((data[offset + n] & value) == value);
            data[offset + n] &= value;
        }
        return !cut;
    }
    bool Erase(size_t offset, size_t bytes) override {
        assert(offset % 4096 == 0 && bytes % 4096 == 0 && offset + bytes <= data.size());
        ++erases;
        if (fail_erase) return false;
        memset(data.data() + offset, 255, bytes); return true;
    }
};
struct Cipher : VoiceCipher {
    std::array<uint8_t,32> key{};
    bool fail_random = false, fail_seal = false;
    Cipher() { assert(RAND_bytes(key.data(), key.size()) == 1); }
    bool NextNonce(uint8_t nonce[12]) override {
        return !fail_random && RAND_bytes(nonce, 12) == 1;
    }
    bool Seal(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes plain,
              uint8_t* cipher, uint8_t tag[16]) override {
        if (fail_seal) return false;
        auto* ctx = EVP_CIPHER_CTX_new(); assert(ctx);
        int length = 0, remaining = 0;
        bool ok = EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), nonce) == 1 &&
            EVP_EncryptUpdate(ctx, nullptr, &length, aad.data, aad.size) == 1 &&
            EVP_EncryptUpdate(ctx, cipher, &length, plain.data, plain.size) == 1 &&
            EVP_EncryptFinal_ex(ctx, cipher + length, &remaining) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag) == 1;
        EVP_CIPHER_CTX_free(ctx); return ok;
    }
    bool Open(const uint8_t nonce[12], VoiceBytes aad, VoiceBytes cipher,
              const uint8_t tag[16], uint8_t* plain) override {
        auto* ctx = EVP_CIPHER_CTX_new(); assert(ctx);
        int length = 0, remaining = 0;
        bool ok = EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), nonce) == 1 &&
            EVP_DecryptUpdate(ctx, nullptr, &length, aad.data, aad.size) == 1 &&
            EVP_DecryptUpdate(ctx, plain, &length, cipher.data, cipher.size) == 1 &&
            EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, const_cast<uint8_t*>(tag)) == 1 &&
            EVP_DecryptFinal_ex(ctx, plain + length, &remaining) == 1;
        EVP_CIPHER_CTX_free(ctx); return ok;
    }
};
struct Buffers {
    std::vector<uint8_t> a = std::vector<uint8_t>(VoiceOutbox::kMaxFrameBytes);
    std::vector<uint8_t> b = std::vector<uint8_t>(VoiceOutbox::kMaxFrameBytes);
};
VoiceCapture capture(int id = 1, int conversation = 2) {
    VoiceCapture value;
    value.request_id[0] = id; value.conversation_id[0] = conversation;
    value.source_request_id[0] = 7; value.source_revision = 12;
    value.packet_count = 2; value.captured_unix_ms = 1788679000123ULL;
    return value;
}
const std::vector<uint8_t> frames = {3,0,0xf8,0xff,0xfe,2,0,0xaa,0xbb};
VoiceBytes bytes(const std::vector<uint8_t>& data) { return {data.data(), data.size()}; }
int main() {
    Flash flash; Cipher cipher; Buffers buffers;
    VoiceOutbox box(flash,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
    SavedVoiceCapture saved;
    assert(box.Read(0,saved) == VoiceStoreResult::Empty);
    assert(box.Save(capture(),bytes(frames),saved) == VoiceStoreResult::Ok);
    assert(saved.slot == 0 && saved.sequence == 1 && saved.capture.captured_unix_ms == capture().captured_unix_ms);
    assert(memcmp(flash.data.data()+VoiceOutbox::kBodyOffset,frames.data(),frames.size()) != 0);
    const auto stable_image = flash.data;
    int writes = flash.writes;
    assert(box.Save(capture(),bytes(frames),saved) == VoiceStoreResult::Ok && flash.writes == writes);
    assert(box.Save(capture(1,3),bytes(frames),saved) == VoiceStoreResult::Conflict);
    auto changed = frames; changed.back() ^= 1;
    assert(box.Save(capture(),bytes(changed),saved) == VoiceStoreResult::Conflict);
    auto changed_source = capture(); ++changed_source.source_request_id[0];
    assert(box.Save(changed_source,bytes(frames),saved) == VoiceStoreResult::Conflict);
    changed_source = capture(); ++changed_source.source_revision;
    assert(box.Save(changed_source,bytes(frames),saved) == VoiceStoreResult::Conflict);
    auto changed_time = capture(); ++changed_time.captured_unix_ms;
    assert(box.Save(changed_time,bytes(frames),saved) == VoiceStoreResult::Conflict);
    Buffers restarted_buffers;
    VoiceOutbox restarted(flash,cipher,restarted_buffers.a.data(),restarted_buffers.b.data(),restarted_buffers.a.size());
    assert(restarted.Read(0,saved) == VoiceStoreResult::Ok);
    assert(saved.capture.source_request_id == capture().source_request_id && saved.capture.source_revision == 12);
    assert(saved.capture.request_id == capture().request_id && saved.capture.conversation_id == capture().conversation_id);
    assert(saved.frames.size == frames.size() && memcmp(saved.frames.data,frames.data(),frames.size()) == 0);
    assert(restarted.RemoveAfterReceipt(0,capture().request_id,capture(1,3).conversation_id) == VoiceStoreResult::Conflict);
    for (int id=2; id<=4; ++id) assert(box.Save(capture(id),bytes(frames),saved) == VoiceStoreResult::Ok);
    const auto full_image = flash.data;
    assert(box.Save(capture(5),bytes(frames),saved) == VoiceStoreResult::Full && flash.data == full_image);
    assert(box.RemoveAfterReceipt(0,capture().request_id,capture().conversation_id) == VoiceStoreResult::Ok);
    assert(box.Save(capture(5),bytes(frames),saved) == VoiceStoreResult::Ok && saved.sequence == 5 && saved.slot == 0);
    assert(box.RemoveAfterReceipt(0,capture().request_id,capture().conversation_id) == VoiceStoreResult::Conflict);
    assert(box.Read(0,saved) == VoiceStoreResult::Ok && saved.capture.request_id == capture(5).request_id);

    // Every torn-header byte and every torn short payload is exercised from a
    // fresh journal object, while the prior complete recording remains exact.
    for (int write=0; write<=1; ++write) for (size_t cut=0; cut < (write ? VoiceOutbox::kHeaderBytes : frames.size()); ++cut) {
        Flash torn; torn.data = stable_image; torn.cut_write = write; torn.cut_bytes = cut;
        VoiceOutbox writer(torn,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
        assert(writer.Save(capture(2),bytes(frames),saved) == VoiceStoreResult::IoError);
        VoiceOutbox reader(torn,cipher,restarted_buffers.a.data(),restarted_buffers.b.data(),restarted_buffers.a.size());
        assert(reader.Read(0,saved) == VoiceStoreResult::Ok && saved.capture.request_id == capture().request_id);
        assert(reader.Read(1,saved) != VoiceStoreResult::Ok);
        assert(std::equal(torn.data.begin(),torn.data.begin()+VoiceOutbox::kSlotBytes,stable_image.begin()));
    }
    // Metadata, nonce, tag and ciphertext tampering must fail authentication.
    for (size_t offset : {size_t(16),size_t(24),size_t(40),size_t(56),size_t(76),size_t(92),size_t(96),size_t(108),VoiceOutbox::kBodyOffset}) {
        Flash bad; bad.data = stable_image; bad.data[offset] ^= 1;
        VoiceOutbox damaged(bad,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
        assert(damaged.Read(0,saved) == VoiceStoreResult::Corrupt);
        const auto quarantined = bad.data;
        assert(damaged.Save(capture(2),bytes(frames),saved) == VoiceStoreResult::Ok && saved.slot == 1);
        assert(std::equal(bad.data.begin(),bad.data.begin()+VoiceOutbox::kSlotBytes,quarantined.begin()));
    }
    // Version three authenticates dictation intent without touching legacy slots.
    auto dictation = capture(20); dictation.purpose = VoicePurpose::Dictation;
    dictation.dictation_session_id[0] = 42; dictation.chunk_sequence = 59; dictation.sample_count = 160000;
    Flash mixed; mixed.data = stable_image;
    VoiceOutbox mixed_box(mixed,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
    assert(mixed_box.Save(dictation,bytes(frames),saved) == VoiceStoreResult::Ok && saved.slot == 1);
    const auto mixed_image = mixed.data;
    assert(std::equal(mixed.data.begin(),mixed.data.begin()+VoiceOutbox::kSlotBytes,stable_image.begin()));
    assert(memcmp(mixed.data.data(),"ORBAUD02",8)==0);
    assert(memcmp(mixed.data.data()+VoiceOutbox::kSlotBytes,"ORBAUD03",8)==0);
    VoiceOutbox mixed_reboot(mixed,cipher,restarted_buffers.a.data(),restarted_buffers.b.data(),restarted_buffers.a.size());
    assert(mixed_reboot.Read(0,saved)==VoiceStoreResult::Ok && !saved.capture.IsDictation() && saved.capture.sample_count==0);
    assert(mixed_reboot.Read(1,saved)==VoiceStoreResult::Ok && saved.capture.IsDictation());
    assert(saved.capture.dictation_session_id==dictation.dictation_session_id && saved.capture.chunk_sequence==59 && saved.capture.sample_count==160000);
    for(int field=0;field<4;++field){auto changed=dictation;
        if(field==0)changed.purpose=VoicePurpose::Command;
        if(field==1)++changed.dictation_session_id[0];
        if(field==2)--changed.chunk_sequence;
        if(field==3)--changed.sample_count;
        auto result=mixed_box.Save(changed,bytes(frames),saved);
        assert(result==(field==0?VoiceStoreResult::Invalid:VoiceStoreResult::Conflict));
        assert(mixed.data==mixed_image);
    }
    for(size_t local:{size_t(8),size_t(96),size_t(100),size_t(116),size_t(120),size_t(124),size_t(136)}){
        Flash bad;bad.data=mixed_image;bad.data[VoiceOutbox::kSlotBytes+local]^=1;
        VoiceOutbox reader(bad,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
        assert(reader.Read(1,saved)==VoiceStoreResult::Corrupt);
    }
    for(int field=0;field<5;++field){auto invalid=dictation;invalid.request_id[0]=21;
        if(field==0)invalid.purpose=static_cast<VoicePurpose>(2);
        if(field==1)invalid.dictation_session_id={};
        if(field==2)invalid.chunk_sequence=60;
        if(field==3)invalid.sample_count=0;
        if(field==4)invalid.sample_count=160001;
        assert(mixed_box.Save(invalid,bytes(frames),saved)==VoiceStoreResult::Invalid && mixed.data==mixed_image);
    }
    for(size_t cut=0;cut<VoiceOutbox::kDictationHeaderBytes;++cut){
        Flash torn;torn.data=stable_image;torn.cut_write=1;torn.cut_bytes=cut;
        VoiceOutbox writer(torn,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
        assert(writer.Save(dictation,bytes(frames),saved)==VoiceStoreResult::IoError);
        VoiceOutbox reader(torn,cipher,restarted_buffers.a.data(),restarted_buffers.b.data(),restarted_buffers.a.size());
        assert(reader.Read(1,saved)!=VoiceStoreResult::Ok);
        assert(reader.Read(0,saved)==VoiceStoreResult::Ok && !saved.capture.IsDictation());
        assert(std::equal(torn.data.begin(),torn.data.begin()+VoiceOutbox::kSlotBytes,stable_image.begin()));
    }
    Cipher other_device;
    VoiceOutbox wrong_key(flash,other_device,buffers.a.data(),buffers.b.data(),buffers.a.size());
    assert(wrong_key.Read(0,saved) == VoiceStoreResult::Corrupt);
    Flash broken; VoiceOutbox errors(broken,cipher,buffers.a.data(),buffers.b.data(),buffers.a.size());
    broken.fail_read = true;
    assert(errors.Save(capture(),bytes(frames),saved) == VoiceStoreResult::IoError && broken.erases == 0);
    broken.fail_read = false; broken.fail_erase = true;
    assert(errors.Save(capture(),bytes(frames),saved) == VoiceStoreResult::IoError && broken.writes == 0);
    broken.fail_erase = false; broken.lie_write = true;
    assert(errors.Save(capture(),bytes(frames),saved) != VoiceStoreResult::Ok);
    broken.lie_write = false; cipher.fail_random = true;
    int erases = broken.erases;
    assert(errors.Save(capture(),bytes(frames),saved) == VoiceStoreResult::CryptoError && broken.erases == erases);
    cipher.fail_random = false; cipher.fail_seal = true;
    assert(errors.Save(capture(),bytes(frames),saved) == VoiceStoreResult::CryptoError && broken.erases == erases);
    cipher.fail_seal = false;
    auto empty_id = capture(); empty_id.request_id = {};
    assert(errors.Save(empty_id,bytes(frames),saved) == VoiceStoreResult::Invalid);
    for (std::vector<uint8_t> malformed : {std::vector<uint8_t>{}, {0,0}, {255,255,1}, {1,0,1,4,0,2}})
        assert(!VoiceOutbox::ValidFrames(bytes(malformed),2));
    assert(!VoiceOutbox::ValidFrames(bytes(frames),1));
    assert(!VoiceOutbox::ValidFrames(bytes(frames),168));
    VoiceOutbox insufficient(broken,cipher,buffers.a.data(),buffers.b.data(),1);
    assert(insufficient.Save(capture(),bytes(frames),saved) == VoiceStoreResult::Invalid);
    std::cout << "Encrypted outbox restart, every torn write, exact receipts, scope and fault checks passed\n";
}
'''
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "test.cc"
            binary = Path(directory) / "test"
            source.write_text(program)
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-I", str(ROOT / "main"), *include,
                            str(ROOT / "main/provisions_voice_outbox.cc"), str(source), *libraries,
                            "-lcrypto", "-o", str(binary)], check=True, capture_output=True)
            result = subprocess.run([str(binary)], check=False, capture_output=True, text=True,
                                    env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("every torn write", result.stdout)


if __name__ == "__main__":
    unittest.main()
