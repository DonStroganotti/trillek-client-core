
#include "util/UtilType.hpp"
#include "util/Compression.hpp"

namespace trillek {
namespace util {
namespace algorithm {

BitStreamDecoder::BitStreamDecoder() {
    indata = DataString();
    inpos = 0;
    num_bits = 0;
    bit_buffer = 0;
}

void_er BitStreamDecoder::AppendData(const DataString & in)
{
    indata.append(in);
    return void_er();
}

ErrorReturn<uint8_t> BitStreamDecoder::ReadByte() {
    if(inpos < indata.length()) {
        return ErrorReturn<uint8_t>(indata[inpos++]);
    } else {
        return ErrorReturn<uint8_t>(0, -1, "Not enough data");
    }
}

void_er BitStreamDecoder::LoadByte() {
    if(num_bits <= 24) {
        if(!(bit_buffer < 1u << num_bits)) {
            return void_er(-2, "Bit buffer corrupt");
        }
        if(inpos < indata.length()) {
            bit_buffer |= indata[inpos++] << num_bits;
            num_bits += 8;
            return void_er(0);
        } else {
            return void_er(1, "Not enough data");
        }
    } else {
        return void_er(0);
    }
}
void_er BitStreamDecoder::Require(uint32_t n) {
    void_er ret;
    if(n > 24) {
        // get the number bytes that would need to be fetched
        n -= num_bits; // excluding what we have fetched
        uint32_t nbytes = n >> 3;
        if(n & 0x7) nbytes++; // non-multiples of 8 bits
        if(indata.length() - inpos < nbytes) {
            return void_er(1, "Not enough data");
        }
    }
    while(num_bits < n) {
        ret = LoadByte();
        if(ret) return ret;
    }
    return void_er(0);
}
void_er BitStreamDecoder::LoadFull() {
    while(num_bits <= 24) {
        if(!(bit_buffer < 1u << num_bits)) {
            return void_er(-2, "Bit buffer corrupt");
        }
        if(inpos < indata.length()) {
            bit_buffer |= indata[inpos++] << num_bits;
            num_bits += 8;
            return void_er(0);
        } else {
            return void_er(1, "Not enough data");
        }
    }
    return void_er(0);
}

ErrorReturn<uint32_t> BitStreamDecoder::GetBits(uint32_t n) {
    if(n > 32) {
        return ErrorReturn<uint32_t>(0, -5, "Invalid input");
    }
    if(num_bits < n) {
        void_er st;
        st = LoadFull();
        if(num_bits < n && st) {
            return st.value<uint32_t>(0u);
        }
    }
    uint32_t rbits = bit_buffer & ((1u << n) - 1);
    bit_buffer >>= n;
    num_bits -= n;
    return ErrorReturn<uint32_t>(rbits);
}

template<typename T, std::size_t max>
static void n_memarrayset(T (&mem)[max], const T value, std::size_t count) {
    if(count > max) count = max;
    for(std::size_t i = 0; i < count; i++) mem[i] = value;
}
template<typename T, std::size_t max>
static void n_memarrayset(T (&mem)[max], std::size_t start, const T value, std::size_t count) {
    if(start > max) return;
    std::size_t end = start + count;
    if(end > max) end = max;
    for(std::size_t i = start; i < end; i++) mem[i] = value;
}
// Inflate decoder Huffman functions are based on the public domain
// zlib decode - Sean Barrett, originally found in SOIL and modified for C++

void_er Huffman::Build(const uint8_t *sizelist, uint32_t num) {
    uint32_t i;
    uint16_t code;
    uint32_t next_code[16], sizes[17];

    // DEFLATE spec for generating codes
    n_memarrayset(sizes, 0u, sizeof(sizes));
    n_memarrayset(this->fast, (uint16_t)0xffffu, sizeof(this->fast));
    for(i = 0; i < num; ++i) {
        if(!(sizelist[i] <= 16)) return void_er(-5, "Bad sizelist");
        ++sizes[sizelist[i]];
    }
    sizes[0] = 0;
    for(i = 1; i < 16; ++i) {
        if(!(sizes[i] <= (1u << i))) {
            return void_er(-5, "Bad sizes");
        }
    }
    code = 0;
    uint16_t symbol = 0;
    for(i = 1; i < 16; ++i) {
        next_code[i] = code;
        firstcode[i] = code;
        firstsymbol[i] = symbol;
        code += sizes[i];
        if(sizes[i]) {
            if(code - 1u >= (1u << i)) {
                return void_er(-5, "Bad codelengths");
            }
        }
        this->maxcode[i] = code << (16 - i); // preshift for inner loop
        code <<= 1;
        symbol += sizes[i];
    }
    this->maxcode[16] = 0x10000; // sentinel
    for(i = 0; i < num; ++i) {
        uint32_t codelen = sizelist[i];
        if(codelen) {
            uint16_t c;
            c = next_code[codelen] - firstcode[codelen] + firstsymbol[codelen];
            this->size[c] = (uint8_t)codelen;
            this->value[c] = (uint16_t)i;
            if(codelen <= 9) {
                uint32_t k = BitReverse(next_code[codelen], codelen);
                while(k < 512) {
                    fast[k] = c;
                    k += (1 << codelen);
                }
            }
            ++next_code[codelen];
        }
    }
    return 1;
}

ErrorReturn<uint16_t> Inflate::HuffmanDecode(const Huffman& z) {
    uint32_t codesize;
    uint32_t b, k;
    void_er st = instream.Require(16);
    if(st) {
        return st.value<uint16_t>(0);
    }
    b = z.fast[instream.bit_buffer & 0x1FF];
    if(b < 0xffff) {
        if(!(b < 288)) {
            return ErrorReturn<uint16_t>(0, -4, "Index out of range");
        }
        codesize = z.size[b];
        instream.GetBits(codesize);
        return ErrorReturn<uint16_t>(z.value[b]);
    }

    // not resolved by fast table, so compute it
    k = BitReverse(instream.bit_buffer, 16);

    // validate
    for(codesize = 10; k >= z.maxcode[codesize] && codesize < 16; ++codesize);
    if(codesize == 16) {
        return ErrorReturn<uint16_t>(0, -3, "Code invalid");
    }

    b = (k >> (16 - codesize)) - z.firstcode[codesize] + z.firstsymbol[codesize];
    if(!(b < 288)) {
        return ErrorReturn<uint16_t>(0, -4, "Index out of range");
    }
    if(!z.size[b] == codesize) {
        return ErrorReturn<uint16_t>(0, -3, "Size table invalid");
    }
    instream.GetBits(codesize);
    return ErrorReturn<uint16_t>(z.value[b]);
}

void_er Inflate::UncompressedBlock() {
    return void_er();
}
void_er Inflate::DynamicBlock() {
    static const uint8_t ALPHABET_LENGTHS[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    std::unique_ptr<Huffman> codelength(new Huffman());
    uint8_t lencodes[286 + 32 + 137];//padding for maximum single op
    uint8_t codelength_sizes[19];
    void_er ret;
    uint32_t i, n;

    ret = instream.Require(14);
    if(ret) return ret;
    uint32_t num_literal_codes  = (ret = instream.GetBits(5)) + 257;
    if(ret) return ret;
    uint32_t num_distance_codes = (ret = instream.GetBits(5)) + 1;
    if(ret) return ret;
    uint32_t num_codelen_codes  = (ret = instream.GetBits(4)) + 4;
    if(ret) return ret;

    n_memarrayset(codelength_sizes, (uint8_t)0, sizeof(codelength_sizes));
    for(i = 0; i < num_codelen_codes; ++i) {
        uint32_t s = (ret = instream.GetBits(3));
        if(ret) return ret;
        codelength_sizes[ALPHABET_LENGTHS[i]] = (uint8_t)s;
    }
    ret = codelength->Build(codelength_sizes, 19);
    if(ret) return ret;

    n = 0;
    while(n < num_literal_codes + num_distance_codes) {
        uint16_t c = (ret = HuffmanDecode(*codelength));
        if(ret) return ret;
        if( !(c >= 0u && c < 19u) ) return void_er(-2, "Invalid decode");
        if(c < 16u) {
            lencodes[n++] = (uint8_t)c;
        }
        else if(c == 16) {
            c = instream.GetBits(2) + 3;
            n_memarrayset(lencodes, n, lencodes[n - 1], c);
            n += c;
        }
        else if(c == 17) {
            c = instream.GetBits(3) + 3;
            n_memarrayset(lencodes, n, (uint8_t)0, c);
            n += c;
        }
        else {
            //assert(c == 18);
            c = instream.GetBits(7) + 11;
            n_memarrayset(lencodes, n, (uint8_t)0, c);
            n += c;
        }
    }
    if(n != num_literal_codes + num_distance_codes) {
        return void_er(-3, "Bad code lengths");
    }
    ret = length.Build(lencodes, num_literal_codes);
    if(ret) return ret;
    ret = distance.Build(lencodes + num_literal_codes, num_distance_codes);
    if(ret) return ret;

    return void_er();
}

void_er Inflate::HuffmanBlock() {

    return void_er();
}

Inflate::Inflate() {
    outdata = DataString();
    outpos = 0;

    errored = false;

    readstate = InflateState::ZLIB_HEADER;
}

Inflate::~Inflate() {
}

bool Inflate::DecompressStart() {
    return false;
}

bool Inflate::DecompressEnd() {
    return false;
}

static const uint8_t default_length[288] = {
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9,
    7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 8
};
static const uint8_t default_distance[32] = {
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5
};

bool Inflate::DecompressData(DataString data) {
    instream.AppendData(data);

    uint32_t final_flag, type;

    readstate = InflateState::BLOCK_HEADER;
    final_flag = 0;
    while(!final_flag && !error_state) {
        final_flag = (error_state = instream.GetBits(1));
        if(error_state) return true;
        type = (error_state = instream.GetBits(2));
        if(error_state) return true;
        switch(type) {
        case 0:
            error_state = UncompressedBlock();
            if(error_state) return true;
            break;
        case 1:
        case 2:
            if(type == 1) {
                // use fixed code lengths
                error_state = length.Build(default_length, 288);
                if(error_state) return true;
                error_state = distance.Build(default_distance, 32);
                if(error_state) return true;
            }
            else {
                error_state = DynamicBlock();
                if(error_state) return true;
            }
            error_state = HuffmanBlock();
            if(error_state) return true;
            break;
        case 3:
            return false;
            break;
        default:
            return true;
            break;
        }
    }

    return false;
}

bool Inflate::DecompressHasOutput() {
    return false;
}

DataString Inflate::DecompressGetOutput() {
    return DataString();
}

} // algorithm
} // util
} // trillek
