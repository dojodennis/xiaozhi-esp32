"""Exercise the actual ESP adapter with fault-injected flash/NVS and real GCM."""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

HEADERS = {
    "esp_partition.h": r'''
#pragma once
#include <cstddef>
#include <cstdint>
using esp_err_t = int;
constexpr int ESP_OK=0, ESP_FAIL=-1, ESP_PARTITION_TYPE_DATA=1;
constexpr int ESP_PARTITION_SUBTYPE_DATA_SPIFFS=130;
struct esp_partition_t { uint32_t address; size_t size; };
const esp_partition_t* esp_partition_find_first(int,int,const char*);
esp_err_t esp_partition_read(const esp_partition_t*,size_t,void*,size_t);
esp_err_t esp_partition_write(const esp_partition_t*,size_t,const void*,size_t);
esp_err_t esp_partition_erase_range(const esp_partition_t*,size_t,size_t);
''',
    "esp_heap_caps.h": r'''
#pragma once
#include <cstddef>
constexpr unsigned MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
void* heap_caps_malloc(size_t,unsigned);
void heap_caps_free(void*);
''',
    "esp_random.h": r'''
#pragma once
#include <cstddef>
void esp_fill_random(void*,size_t);
''',
    "nvs.h": r'''
#pragma once
#include "esp_partition.h"
using nvs_handle_t = unsigned;
constexpr int NVS_READWRITE=1, NVS_READONLY=0, ESP_ERR_NVS_NOT_FOUND=2;
constexpr int ESP_ERR_NVS_INVALID_LENGTH=3;
esp_err_t nvs_open(const char*,int,nvs_handle_t*);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_blob(nvs_handle_t,const char*,void*,size_t*);
esp_err_t nvs_set_blob(nvs_handle_t,const char*,const void*,size_t);
esp_err_t nvs_get_u64(nvs_handle_t,const char*,uint64_t*);
esp_err_t nvs_set_u64(nvs_handle_t,const char*,uint64_t);
esp_err_t nvs_commit(nvs_handle_t);
''',
    "psa/crypto.h": r'''
#pragma once
#include <cstddef>
#include <cstdint>
using psa_status_t=int;
using psa_key_id_t=unsigned;
constexpr int PSA_SUCCESS=0, PSA_ERROR_GENERIC_ERROR=-1;
constexpr unsigned PSA_KEY_USAGE_ENCRYPT=1, PSA_KEY_USAGE_DECRYPT=2;
constexpr unsigned PSA_ALG_GCM=10, PSA_KEY_TYPE_AES=20, PSA_KEY_ID_NULL=0;
struct psa_key_attributes_t {unsigned usage=0, algorithm=0, type=0, bits=0;};
struct psa_aead_operation_t {void* ctx=nullptr;bool encrypt=false;uint8_t key[32]{};};
#define PSA_KEY_ATTRIBUTES_INIT {}
#define PSA_AEAD_OPERATION_INIT {}
#define PSA_AEAD_UPDATE_OUTPUT_SIZE(type,alg,size) ((size)+15)
int psa_crypto_init();
void psa_set_key_usage_flags(psa_key_attributes_t*,unsigned);
void psa_set_key_algorithm(psa_key_attributes_t*,unsigned);
void psa_set_key_type(psa_key_attributes_t*,unsigned);
void psa_set_key_bits(psa_key_attributes_t*,unsigned);
void psa_reset_key_attributes(psa_key_attributes_t*);
int psa_import_key(const psa_key_attributes_t*,const uint8_t*,size_t,psa_key_id_t*);
int psa_destroy_key(psa_key_id_t);
int psa_aead_encrypt_setup(psa_aead_operation_t*,psa_key_id_t,unsigned);
int psa_aead_decrypt_setup(psa_aead_operation_t*,psa_key_id_t,unsigned);
int psa_aead_set_lengths(psa_aead_operation_t*,size_t,size_t);
int psa_aead_set_nonce(psa_aead_operation_t*,const uint8_t*,size_t);
int psa_aead_update_ad(psa_aead_operation_t*,const uint8_t*,size_t);
int psa_aead_update(psa_aead_operation_t*,const uint8_t*,size_t,uint8_t*,size_t,size_t*);
int psa_aead_finish(psa_aead_operation_t*,uint8_t*,size_t,size_t*,uint8_t*,size_t,size_t*);
int psa_aead_verify(psa_aead_operation_t*,uint8_t*,size_t,size_t*,const uint8_t*,size_t);
int psa_aead_abort(psa_aead_operation_t*);
''',
    "mbedtls/platform_util.h": r'''
#pragma once
#include <cstddef>
void mbedtls_platform_zeroize(void*,size_t);
''',
}

PROGRAM = r'''
#include "provisions_voice_outbox_esp.h"
#include "esp_heap_caps.h"
#include "nvs.h"
#include "psa/crypto.h"
#include <openssl/evp.h>
#include <algorithm>
#include <cassert>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>
using namespace provisions;
constexpr size_t tail=EspVoiceOutbox::kAssetLimit, store=VoiceOutbox::kStoreBytes;
struct State {
    esp_partition_t partition{0x800000,tail+store};
    std::vector<uint8_t> flash=std::vector<uint8_t>(tail+store,255);
    std::map<std::string,std::vector<uint8_t>> nvs,pending;
    std::map<std::string,int> calls;
    std::map<void*,size_t> allocations;
    std::map<unsigned,std::vector<uint8_t>> keys;int active_ops=0;unsigned next_key=1;
    std::string fail; int fail_n=1;
    size_t tail_read=0; int writes=0,erases=0,randoms=0;
    unsigned handles=0; bool absent_partition=false, bad_readback=false, bad_counter_readback=false;
    bool bad(const std::string& operation) {
        int n=++calls[operation]; return fail==operation && n==fail_n;
    }
};
State state;
void put32(size_t offset,uint32_t value) {
    for(int i=0;i<4;i++) state.flash[offset+i]=uint8_t(value>>(8*i));
}
void reset() {
    assert(state.allocations.empty() && state.keys.empty() && state.active_ops==0); state=State{};
    put32(0,1); put32(8,48); // 44-byte table, ZZ prefix, 2-byte payload.
    memset(state.flash.data()+12,0,44); memcpy(state.flash.data()+12,"sample",6);
    put32(44,2); put32(48,0); state.flash[56]='Z';state.flash[57]='Z';
    state.flash[58]=3;state.flash[59]=4;
}
const esp_partition_t* esp_partition_find_first(int type,int subtype,const char* label) {
    assert(type==ESP_PARTITION_TYPE_DATA && subtype==ESP_PARTITION_SUBTYPE_DATA_SPIFFS);
    assert(std::string(label)=="assets");
    return state.absent_partition?nullptr:&state.partition;
}
esp_err_t esp_partition_read(const esp_partition_t* p,size_t offset,void* out,size_t bytes) {
    assert(p==&state.partition && offset<=state.flash.size() && bytes<=state.flash.size()-offset);
    if(state.bad("read")) return ESP_FAIL;
    if(offset>=tail) state.tail_read+=bytes;
    memcpy(out,state.flash.data()+offset,bytes);return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t* p,size_t offset,const void* in,size_t bytes) {
    assert(p==&state.partition && offset>=tail && offset<=tail+store && bytes<=tail+store-offset);
    ++state.writes;if(state.bad("write"))return ESP_FAIL;
    auto* input=static_cast<const uint8_t*>(in);
    for(size_t i=0;i<bytes;i++){assert((state.flash[offset+i]&input[i])==input[i]);state.flash[offset+i]&=input[i];}
    return ESP_OK;
}
esp_err_t esp_partition_erase_range(const esp_partition_t* p,size_t offset,size_t bytes) {
    assert(p==&state.partition && offset>=tail && offset<=tail+store && bytes<=tail+store-offset);
    assert(offset%4096==0 && bytes%4096==0);++state.erases;
    if(state.bad("erase"))return ESP_FAIL;
    memset(state.flash.data()+offset,255,bytes);return ESP_OK;
}
void* heap_caps_malloc(size_t bytes,unsigned caps) {
    assert(bytes==VoiceOutbox::kMaxFrameBytes && caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    if(state.bad("alloc"))return nullptr;
    void* p=malloc(bytes);assert(p);memset(p,0x7c,bytes);state.allocations[p]=bytes;return p;
}
void heap_caps_free(void* p) {
    if(!p)return;
    auto found=state.allocations.find(p);assert(found!=state.allocations.end());
    auto* bytes=static_cast<uint8_t*>(p);
    assert(std::all_of(bytes,bytes+found->second,[](uint8_t c){return c==0;}));
    state.allocations.erase(found);free(p);
}
void esp_fill_random(void* p,size_t bytes) {
    ++state.randoms;auto* out=static_cast<uint8_t*>(p);
    for(size_t i=0;i<bytes;i++)out[i]=uint8_t(13+i+state.randoms);
}
esp_err_t nvs_open(const char* space,int mode,nvs_handle_t* handle) {
    assert(std::string(space)=="orbit_audio" && (mode==NVS_READONLY||mode==NVS_READWRITE));
    if(state.bad("open"))return ESP_FAIL;*handle=++state.handles;return ESP_OK;
}
void nvs_close(nvs_handle_t) {}
esp_err_t nvs_get_blob(nvs_handle_t,const char* key,void* out,size_t* bytes) {
    if(state.bad("get_blob"))return ESP_FAIL;
    auto found=state.nvs.find(key);if(found==state.nvs.end())return ESP_ERR_NVS_NOT_FOUND;
    auto requested=*bytes;*bytes=found->second.size();
    if(requested<*bytes)return ESP_ERR_NVS_INVALID_LENGTH;
    memcpy(out,found->second.data(),*bytes);
    if(state.bad_readback && state.calls["get_blob"]>1)static_cast<uint8_t*>(out)[0]^=1;
    return ESP_OK;
}
esp_err_t nvs_set_blob(nvs_handle_t,const char* key,const void* data,size_t bytes) {
    if(state.bad("set_blob"))return ESP_FAIL;
    auto* p=static_cast<const uint8_t*>(data);state.pending[key]={p,p+bytes};return ESP_OK;
}
esp_err_t nvs_get_u64(nvs_handle_t,const char* key,uint64_t* out) {
    if(state.bad("get_u64"))return ESP_FAIL;
    auto found=state.nvs.find(key);if(found==state.nvs.end())return ESP_ERR_NVS_NOT_FOUND;
    assert(found->second.size()==8);memcpy(out,found->second.data(),8);
    if(state.bad_counter_readback && state.calls["get_u64"]%2==0)++*out;
    return ESP_OK;
}
esp_err_t nvs_set_u64(nvs_handle_t,const char* key,uint64_t value) {
    if(state.bad("set_u64"))return ESP_FAIL;
    auto* p=reinterpret_cast<const uint8_t*>(&value);state.pending[key]={p,p+8};return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if(state.bad("commit")) {state.pending.clear();return ESP_FAIL;}
    for(auto& item:state.pending)state.nvs[item.first]=item.second;
    state.pending.clear();return ESP_OK;
}
void mbedtls_platform_zeroize(void* p,size_t bytes) { memset(p,0,bytes); }

int psa_crypto_init() {return state.bad("crypto_init")?-1:0;}
void psa_set_key_usage_flags(psa_key_attributes_t* a,unsigned value){a->usage=value;}
void psa_set_key_algorithm(psa_key_attributes_t* a,unsigned value){a->algorithm=value;}
void psa_set_key_type(psa_key_attributes_t* a,unsigned value){a->type=value;}
void psa_set_key_bits(psa_key_attributes_t* a,unsigned value){a->bits=value;}
void psa_reset_key_attributes(psa_key_attributes_t* a){*a={};}
int psa_import_key(const psa_key_attributes_t* a,const uint8_t* key,size_t size,psa_key_id_t* id) {
    assert(a->usage==(PSA_KEY_USAGE_ENCRYPT|PSA_KEY_USAGE_DECRYPT));
    assert(a->algorithm==PSA_ALG_GCM && a->type==PSA_KEY_TYPE_AES && a->bits==256 && size==32);
    if(state.bad("import"))return -1;
    *id=state.next_key++;state.keys[*id]={key,key+size};return 0;
}
int psa_destroy_key(psa_key_id_t id) {
    assert(state.keys.erase(id)==1);return state.bad("destroy")?-1:0;
}
int setup(psa_aead_operation_t* op,psa_key_id_t id,unsigned alg,bool encrypt) {
    assert(alg==PSA_ALG_GCM && op->ctx==nullptr);if(state.bad("setup"))return -1;
    op->ctx=EVP_CIPHER_CTX_new();assert(op->ctx);op->encrypt=encrypt;++state.active_ops;
    memcpy(op->key,state.keys.at(id).data(),32);return 0;
}
int psa_aead_encrypt_setup(psa_aead_operation_t* op,psa_key_id_t id,unsigned alg){return setup(op,id,alg,true);}
int psa_aead_decrypt_setup(psa_aead_operation_t* op,psa_key_id_t id,unsigned alg){return setup(op,id,alg,false);}
int psa_aead_set_lengths(psa_aead_operation_t* op,size_t aad,size_t bytes){
    assert(op->ctx && aad==88 && bytes<=VoiceOutbox::kMaxFrameBytes);return state.bad("lengths")?-1:0;
}
int psa_aead_set_nonce(psa_aead_operation_t* op,const uint8_t* nonce,size_t size) {
    assert(size==12);if(state.bad("nonce"))return -1;
    auto* ctx=static_cast<EVP_CIPHER_CTX*>(op->ctx);
    int result=op->encrypt?EVP_EncryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,op->key,nonce):
        EVP_DecryptInit_ex(ctx,EVP_aes_256_gcm(),nullptr,op->key,nonce);
    return result==1?0:-1;
}
int psa_aead_update_ad(psa_aead_operation_t* op,const uint8_t* aad,size_t size) {
    if(state.bad("aad"))return -1;int n=0;auto* ctx=static_cast<EVP_CIPHER_CTX*>(op->ctx);
    int result=op->encrypt?EVP_EncryptUpdate(ctx,nullptr,&n,aad,int(size)):
        EVP_DecryptUpdate(ctx,nullptr,&n,aad,int(size));return result==1?0:-1;
}
int psa_aead_update(psa_aead_operation_t* op,const uint8_t* input,size_t size,
                    uint8_t* output,size_t capacity,size_t* written) {
    assert(size<=1024 && capacity>=size && capacity<=1040);
    if(state.bad("update"))return -1;int n=0;auto* ctx=static_cast<EVP_CIPHER_CTX*>(op->ctx);
    int result=op->encrypt?EVP_EncryptUpdate(ctx,output,&n,input,int(size)):
        EVP_DecryptUpdate(ctx,output,&n,input,int(size));*written=n;
    return result==1?0:-1;
}
int psa_aead_finish(psa_aead_operation_t* op,uint8_t* output,size_t capacity,
                    size_t* written,uint8_t* tag,size_t tag_capacity,size_t* tag_written) {
    assert(op->encrypt && capacity>=16 && tag_capacity==16);
    if(state.bad("finish"))return -1;auto* ctx=static_cast<EVP_CIPHER_CTX*>(op->ctx);int n=0;
    bool ok=EVP_EncryptFinal_ex(ctx,output,&n)==1 && EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_GET_TAG,16,tag)==1;
    *written=n;*tag_written=state.bad("short_tag")?15:16;return ok?0:-1;
}
int psa_aead_verify(psa_aead_operation_t* op,uint8_t* output,size_t capacity,
                    size_t* written,const uint8_t* tag,size_t tag_size) {
    assert(!op->encrypt && capacity>=16 && tag_size==16);
    if(state.bad("verify"))return -1;auto* ctx=static_cast<EVP_CIPHER_CTX*>(op->ctx);int n=0;
    bool ok=EVP_CIPHER_CTX_ctrl(ctx,EVP_CTRL_GCM_SET_TAG,16,const_cast<uint8_t*>(tag))==1 &&
        EVP_DecryptFinal_ex(ctx,output,&n)==1;*written=n;return ok?0:-1;
}
int psa_aead_abort(psa_aead_operation_t* op) {
    if(op->ctx){EVP_CIPHER_CTX_free(static_cast<EVP_CIPHER_CTX*>(op->ctx));--state.active_ops;}
    *op={};return 0;
}
VoiceCapture capture(int id) {VoiceCapture c;c.request_id[0]=id;c.conversation_id[0]=42;c.packet_count=1;return c;}
const uint8_t frame[]={2,0,0xab,0xcd};
VoiceStoreResult save(EspVoiceOutbox& box,int id,SavedVoiceCapture& out) {
    return box.journal()->Save(capture(id),{frame,sizeof(frame)},out);
}
void no_initial_writes() {assert(state.writes==0 && state.erases==0);}
int main() {
    reset();
    {EspVoiceOutbox box;assert(!box.Initialize() && box.journal()==nullptr);}
    assert(state.nvs.empty() && state.randoms==0);no_initial_writes();
    // The complete reserved region must be checked before a missing key is created.
    for(size_t position:{size_t(0),size_t(4095),store/2,store-512*1024-1,store-1}) {
        reset();state.flash[tail+position]=0;auto before=state.flash;
        {EspVoiceOutbox box;assert(!box.Initialize(true) && box.journal()==nullptr);}
        assert(state.nvs.empty() && state.calls["set_blob"]==0 && state.flash==before);no_initial_writes();
    }
    // Exact assets partition, packed asset body bounds, overflow and name termination.
    for(int bad=0;bad<8;bad++) {
        reset();
        if(bad==0)state.absent_partition=true;
        if(bad==1)state.partition.address++;
        if(bad==2)state.partition.size--;
        if(bad==3)put32(8,tail-11);
        if(bad==4)put32(44,3); // Declared extent exceeds body by one.
        if(bad==5)put32(48,UINT32_MAX);
        if(bad==6)memset(state.flash.data()+12,'x',32);
        if(bad==7)put32(0,1025);
        {EspVoiceOutbox box;assert(!box.Initialize(true));}
        no_initial_writes();assert(state.nvs.empty() && state.calls["alloc"]==0);
    }
    reset();put32(8,tail-12);put32(44,tail-58);
    {EspVoiceOutbox box;assert(box.Initialize(true));assert(state.tail_read==store);no_initial_writes();}
    // Allocation, storage and readback errors leave audio flash unchanged.
    for(const std::string operation:{"read","alloc","open","get_blob","set_blob","set_u64","commit"}) {
        reset();state.fail=operation;auto before=state.flash;
        {EspVoiceOutbox box;assert(!box.Initialize(true));assert(box.journal()==nullptr);}
        assert(state.flash==before);no_initial_writes();
    }
    reset();state.fail="alloc";state.fail_n=2;
    {EspVoiceOutbox box;assert(!box.Initialize(true));assert(state.nvs.empty());
      state.fail.clear();assert(box.Initialize(true));assert(state.calls["alloc"]==3);}
    reset();state.bad_readback=true;
    {EspVoiceOutbox box;assert(!box.Initialize(true));}no_initial_writes();
    for(size_t size:{size_t(0),size_t(31),size_t(33)}) {
        reset();state.nvs["key_v1"]=std::vector<uint8_t>(size,13);
        {EspVoiceOutbox box;assert(!box.Initialize(true));}
        assert(state.calls["set_blob"]==0);no_initial_writes();
    }
    // Real authenticated restart/read/remove operations stay entirely beyond assets.
    reset();std::vector<uint8_t> assets(state.flash.begin(),state.flash.begin()+tail);
    SavedVoiceCapture out;
    {EspVoiceOutbox box;assert(box.Initialize(true));no_initial_writes();
      assert(save(box,1,out)==VoiceStoreResult::Ok);
      assert(memcmp(state.flash.data()+tail+VoiceOutbox::kBodyOffset,frame,sizeof(frame))!=0);}
    auto key=state.nvs.at("key_v1");int randoms=state.randoms;
    {EspVoiceOutbox box;assert(box.Initialize(true));assert(state.nvs.at("key_v1")==key);
      assert(box.journal()->Read(0,out)==VoiceStoreResult::Ok);
      assert(out.capture.request_id==capture(1).request_id && out.frames.size==sizeof(frame));
      assert(memcmp(out.frames.data,frame,sizeof(frame))==0);
      for(int id=2;id<=4;id++)assert(save(box,id,out)==VoiceStoreResult::Ok);
      assert(save(box,5,out)==VoiceStoreResult::Full);
      for(size_t slot=0;slot<4;slot++)assert(box.journal()->RemoveAfterReceipt(slot,capture(slot+1).request_id,capture(1).conversation_id)==VoiceStoreResult::Ok);
    }
    assert(std::equal(assets.begin(),assets.end(),state.flash.begin()));
    assert(state.randoms==randoms); // Cold-boot saves use no new random entropy.
    // Nonces are consumed once, including encryption or persistence failures.
    reset();
    {EspVoiceOutbox box;assert(box.Initialize(true));assert(save(box,1,out)==VoiceStoreResult::Ok);
      assert(memcmp(state.flash.data()+tail+76,"ORB1\1\0\0\0\0\0\0\0",12)==0);
      auto counter=state.nvs.at("nonce_v1");int commits=state.calls["commit"];
      assert(save(box,1,out)==VoiceStoreResult::Ok);
      assert(state.nvs.at("nonce_v1")==counter && state.calls["commit"]==commits);}
    {EspVoiceOutbox box;assert(box.Initialize());assert(save(box,2,out)==VoiceStoreResult::Ok);
      assert(memcmp(state.flash.data()+tail+VoiceOutbox::kSlotBytes+76,"ORB1\2\0\0\0\0\0\0\0",12)==0);}
    for(const std::string operation:{"open","get_u64","set_u64","commit"}) {
        reset();
        {EspVoiceOutbox box;assert(box.Initialize(true));state.calls.clear();state.fail=operation;
          assert(save(box,1,out)==VoiceStoreResult::CryptoError);no_initial_writes();
          state.fail.clear();assert(save(box,1,out)==VoiceStoreResult::Ok);}
    }
    reset();
    {EspVoiceOutbox box;assert(box.Initialize(true));state.calls.clear();state.bad_counter_readback=true;
      assert(save(box,1,out)==VoiceStoreResult::CryptoError);no_initial_writes();
      state.bad_counter_readback=false;assert(save(box,1,out)==VoiceStoreResult::Ok);
      assert(state.flash[tail+80]==2);}
    reset();
    {EspVoiceOutbox box;assert(box.Initialize(true));state.calls.clear();state.fail="update";
      assert(save(box,1,out)==VoiceStoreResult::CryptoError);no_initial_writes();
      state.fail.clear();assert(save(box,1,out)==VoiceStoreResult::Ok);assert(state.flash[tail+80]==2);}
    // Lost counter permits reading old ciphertext, but cannot authorize another nonce.
    state.nvs.erase("nonce_v1");state.calls.clear();state.writes=state.erases=0;
    {EspVoiceOutbox box;assert(box.Initialize());assert(box.journal()->Read(0,out)==VoiceStoreResult::Ok);
      assert(save(box,2,out)==VoiceStoreResult::CryptoError);no_initial_writes();
      assert(state.calls["set_u64"]==0);}
    state.nvs["nonce_v1"]=std::vector<uint8_t>(8,255);
    {EspVoiceOutbox box;assert(box.Initialize());assert(save(box,2,out)==VoiceStoreResult::CryptoError);
      no_initial_writes();assert(state.calls["set_u64"]==0);}
    // Lost key with ciphertext must never destroy either the record or nonce state.
    state.nvs.erase("key_v1");auto saved_image=state.flash;auto saved_nvs=state.nvs;
    {EspVoiceOutbox box;assert(!box.Initialize(true));}
    assert(state.flash==saved_image && state.nvs==saved_nvs);no_initial_writes();
    for(const std::string operation:{"crypto_init","import","setup","lengths","nonce","aad","update","finish","short_tag","destroy","erase","write","verify"}) {
        reset();
        {EspVoiceOutbox box;assert(box.Initialize(true));state.fail=operation;
          auto result=save(box,1,out);assert(result!=VoiceStoreResult::Ok);
          if(operation!="erase" && operation!="write" && operation!="verify")assert(state.erases==0 && state.writes==0);
          if(operation=="verify")for(auto& allocation:state.allocations) {
            auto* p=static_cast<uint8_t*>(allocation.first);
            assert(!(p[0]==0x7c && p[1]==0x7c && p[2]==0x7c && p[3]==0x7c));
          }
        }
    }

    // Maximum-length recordings exercise all 335 update chunks and their final short chunk.
    reset();std::vector<uint8_t> longest(VoiceOutbox::kMaxFrameBytes,0x8a);
    for(size_t i=0;i<VoiceOutbox::kMaxPackets;i++) {
        longest[i*2050]=0;longest[i*2050+1]=8;
    }
    auto large=capture(1);large.packet_count=VoiceOutbox::kMaxPackets;
    {EspVoiceOutbox box;assert(box.Initialize(true));
      assert(box.journal()->Save(large,{longest.data(),longest.size()},out)==VoiceStoreResult::Ok);
      assert(out.frames.size==longest.size() && memcmp(out.frames.data,longest.data(),longest.size())==0);
      state.calls.clear();state.fail="update";state.fail_n=2;
      assert(box.journal()->Read(0,out)==VoiceStoreResult::Corrupt && out.frames.data==nullptr);
      assert(state.keys.empty() && state.active_ops==0);
      state.fail.clear();assert(box.journal()->Read(0,out)==VoiceStoreResult::Ok);
    }
    assert(state.keys.empty() && state.active_ops==0);
    std::cout<<"ESP adapter bounds, persistence, restart, and fault cases passed\n";
}
'''


class EspVoiceOutboxReviewTests(unittest.TestCase):
    def test_actual_adapter_flash_nvs_crypto_failure_boundaries(self):
        compiler = shutil.which("c++")
        self.assertIsNotNone(compiler)
        crypto = Path("/opt/homebrew/opt/openssl@3")
        flags = ["-I", str(crypto / "include"), "-L", str(crypto / "lib")] if crypto.exists() else []
        with tempfile.TemporaryDirectory(prefix="orbit-esp-adapter-review-") as folder:
            path = Path(folder)
            for name, source in HEADERS.items():
                header = path / name
                header.parent.mkdir(parents=True, exist_ok=True)
                header.write_text(source)
            (path / "review.cc").write_text(PROGRAM)
            binary = path / "review"
            subprocess.run([compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                            "-I", str(path), "-I", str(ROOT / "main"), *flags,
                            str(path / "review.cc"), str(ROOT / "main/provisions_voice_outbox.cc"),
                            str(ROOT / "main/provisions_voice_outbox_esp.cc"), "-lcrypto",
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0" if sys.platform == "darwin" else "detect_leaks=1"})


if __name__ == "__main__":
    unittest.main()
