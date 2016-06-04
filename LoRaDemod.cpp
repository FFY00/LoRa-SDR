// Copyright (c) 2016-2016 Lime Microsystems
// SPDX-License-Identifier: BSL-1.0

#include <Pothos/Framework.hpp>
#include <iostream>
#include <complex>
#include <cmath>
#include "LoRaDetector.hpp"

/***********************************************************************
 * |PothosDoc LoRa Demod
 *
 * Demodulate LoRa packets from a complex sample stream into symbols.
 *
 * <h2>Input format</h2>
 *
 * The input port 0 accepts a complex sample stream of modulated chirps
 * received at the specified bandwidth and carrier frequency.
 *
 * <h2>Output format</h2>
 *
 * The output port 0 produces a packet containing demodulated symbols.
 * The format of the packet payload is a buffer of unsigned shorts.
 * A 16-bit short can fit all size symbols from 7 to 12 bits.
 *
 * <h2>Debug port raw</h2>
 *
 * The raw debug port outputs the LoRa signal annotated with labels
 * for important synchronization points in the input sample stream.
 *
 * <h2>Debug port dec</h2>
 *
 * The dec debug port outputs the LoRa signal downconverted
 * by a locally generated chirp with the same annotation labels as the raw output.
 *
 * |category /LoRa
 * |keywords lora
 *
 * |param sf[Spread factor] The spreading factor controls the symbol spread.
 * Each symbol will occupy 2^SF number of samples given the waveform BW.
 * |default 8
 *
 * |param sync[Sync word] The sync word is a 2-nibble, 2-symbol sync value.
 * The sync word is encoded after the up-chirps and before the down-chirps.
 * The demodulator ignores packets that do not match the sync word.
 * |default 0x12
 *
 * |param mtu[Symbol MTU] Produce MTU symbols after sync is found.
 * The demodulator does not inspect the payload and will simply
 * produce the specified number of symbols once synchronized.
 * |default 256
 *
 * |factory /lora/lora_demod(sf)
 * |setter setSync(sync)
 * |setter setMTU(mtu)
 **********************************************************************/
class LoRaDemod : public Pothos::Block
{
public:
    LoRaDemod(const size_t sf):
        N(1 << sf),
        _detector(N),
        _sync(0x12),
        _mtu(256)
    {
        this->registerCall(this, POTHOS_FCN_TUPLE(LoRaDemod, setSync));
        this->registerCall(this, POTHOS_FCN_TUPLE(LoRaDemod, setMTU));
        this->setupInput(0, typeid(std::complex<float>));
        this->setupOutput(0);
        this->setupOutput("raw", typeid(std::complex<float>));
        this->setupOutput("dec", typeid(std::complex<float>));

        //use at most two input symbols available
        this->input(0)->setReserve(N*2);

        //store port pointers to avoid lookup by name
        _rawPort = this->output("raw");
        _decPort = this->output("dec");

        //generate chirp table
        double phaseAccum = 0.0;
        for (size_t i = 0; i < N; i++)
        {
            double phase = (2*(i+N/2)*M_PI)/N;
            phaseAccum += phase;
            auto entry = std::polar(1.0, phaseAccum);
            _upChirpTable.push_back(std::complex<float>(std::conj(entry)));
            _downChirpTable.push_back(std::complex<float>(entry));
        }
    }

    static Block *make(const size_t sf)
    {
        return new LoRaDemod(sf);
    }

    void setSync(const unsigned char sync)
    {
        _sync = sync;
    }

    void setMTU(const size_t mtu)
    {
        _mtu = mtu;
    }

    void activate(void)
    {
        _state = STATE_FRAMESYNC;
        _chirpTable = _upChirpTable.data();
    }

    void work(void)
    {
        auto inPort = this->input(0);
        if (inPort->elements() < N*2) return;
        if (_rawPort->elements() < N*2)
        {
            _rawPort->popBuffer(_rawPort->elements());
            return;
        }
        if (_decPort->elements() < N*2)
        {
            _decPort->popBuffer(_decPort->elements());
            return;
        }

        size_t total = 0;
        auto inBuff = inPort->buffer().as<const std::complex<float> *>();
        auto rawBuff = _rawPort->buffer().as<std::complex<float> *>();
        auto decBuff = _decPort->buffer().as<std::complex<float> *>();

        //process the available symbol
        for (size_t i = 0; i < N; i++)
        {
            auto samp = inBuff[i];
            auto decd = samp*_chirpTable[i];
            rawBuff[i] = samp;
            decBuff[i] = decd;
            _detector.feed(i, decd);
        }
        auto value = _detector.detect();

        switch (_state)
        {
        ////////////////////////////////////////////////////////////////
        case STATE_FRAMESYNC:
        ////////////////////////////////////////////////////////////////
        {
            //format as observed from inspecting RN2483
            bool syncd = (_prevValue+1)/2 == 0;
            bool match0 = (value+4)/8 == unsigned(_sync>>4);
            bool match1 = false;

            //if the symbol matches sync word0 then check sync word1 as well
            //otherwise assume its the frame sync and adjust for frequency error
            if (syncd and match0)
            {
                for (size_t i = 0; i < N; i++)
                {
                    auto samp = inBuff[i + N];
                    auto decd = samp*_chirpTable[i];
                    rawBuff[i+N] = samp;
                    decBuff[i+N] = decd;
                    _detector.feed(i, decd);
                }
                auto value1 = _detector.detect();
                //format as observed from inspecting RN2483
                match1 = (value1+4)/8 == (_sync & 0xf);
            }

            if (syncd and match0 and match1)
            {
                total = 2*N;
                _state = STATE_DOWNCHIRP0;
                _chirpTable = _downChirpTable.data();
                _id = "SYNC";
            }

            //otherwise its a frequency error
            else
            {
                total = N - value;
                _id = "X";
            }

        } break;

        ////////////////////////////////////////////////////////////////
        case STATE_DOWNCHIRP0:
        ////////////////////////////////////////////////////////////////
        {
            _state = STATE_DOWNCHIRP1;
            total = N;
            _id = "DC";
        } break;

        ////////////////////////////////////////////////////////////////
        case STATE_DOWNCHIRP1:
        ////////////////////////////////////////////////////////////////
        {
            _state = STATE_QUARTERCHIRP;
            total = N;
            _chirpTable = _upChirpTable.data();
            _id = "";
            _outSymbols = Pothos::BufferChunk(typeid(int16_t), _mtu);
        } break;

        ////////////////////////////////////////////////////////////////
        case STATE_QUARTERCHIRP:
        ////////////////////////////////////////////////////////////////
        {
            _state = STATE_DATASYMBOLS;
            total = N/4;
            _symCount = 0;
            _id = "QC";
        } break;

        ////////////////////////////////////////////////////////////////
        case STATE_DATASYMBOLS:
        ////////////////////////////////////////////////////////////////
        {
            total = N;
            _outSymbols.as<int16_t *>()[_symCount] = int16_t(value);
            _symCount++;
            if (_symCount >= _mtu)
            {
                Pothos::Packet pkt;
                pkt.payload = _outSymbols;
                this->output(0)->postMessage(pkt);
                _state = STATE_FRAMESYNC;
            }
            _id = "S";
        } break;

        }

        if (not _id.empty())
        {
            _rawPort->postLabel(Pothos::Label(_id, Pothos::Object(), 0));
            _decPort->postLabel(Pothos::Label(_id, Pothos::Object(), 0));
        }
        inPort->consume(total);
        _rawPort->produce(total);
        _decPort->produce(total);
        _prevValue = value;
    }

    //! Custom output buffer manager with slabs large enough for debug output
    Pothos::BufferManager::Sptr getOutputBufferManager(const std::string &name, const std::string &domain)
    {
        if (name == "raw" or name == "dec")
        {
            Pothos::BufferManagerArgs args;
            args.bufferSize = N*2*sizeof(std::complex<float>);
            return Pothos::BufferManager::make("generic", args);
        }
        return Pothos::Block::getOutputBufferManager(name, domain);
    }

    //! Custom input buffer manager with slabs large enough for fft input
    Pothos::BufferManager::Sptr getInputBufferManager(const std::string &name, const std::string &domain)
    {
        if (name == "raw" or name == "dec")
        {
            Pothos::BufferManagerArgs args;
            args.bufferSize = N*2*sizeof(std::complex<float>);
            return Pothos::BufferManager::make("circular", args);
        }
        return Pothos::Block::getInputBufferManager(name, domain);
    }

private:
    //configuration
    const size_t N;
    LoRaDetector<float> _detector;
    std::complex<float> *_chirpTable;
    std::vector<std::complex<float>> _upChirpTable;
    std::vector<std::complex<float>> _downChirpTable;
    unsigned char _sync;
    size_t _mtu;
    Pothos::OutputPort *_rawPort;
    Pothos::OutputPort *_decPort;

    //state
    enum LoraDemodState
    {
        STATE_FRAMESYNC,
        STATE_DOWNCHIRP0,
        STATE_DOWNCHIRP1,
        STATE_QUARTERCHIRP,
        STATE_DATASYMBOLS,
    };
    LoraDemodState _state;
    size_t _symCount;
    Pothos::BufferChunk _outSymbols;
    std::string _id;
    short _prevValue;
};

static Pothos::BlockRegistry registerLoRaDemod(
    "/lora/lora_demod", &LoRaDemod::make);
