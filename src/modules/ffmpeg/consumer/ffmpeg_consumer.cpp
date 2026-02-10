/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "ffmpeg_consumer.h"

#include "../util/av_assert.h"
#include "../util/av_util.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/memory.h>
#include <common/scope_exit.h>
#include <common/timer.h>

#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/regex.hpp>

#pragma warning(push)
#pragma warning(disable : 4244)
#pragma warning(disable : 4245)
#include <boost/crc.hpp>
#pragma warning(pop)

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libswscale/swscale.h>
}
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include <tbb/concurrent_queue.h>
#include <tbb/parallel_for.h>
#include <tbb/parallel_invoke.h>

#include <memory>
#include <thread>
#include <ctime>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#endif
#include <chrono>
#include <iomanip>

namespace caspar { namespace ffmpeg {

// Helper function para obter uso de memória do processo (em MB)
inline double get_process_memory_mb() {
#ifdef _WIN32
    PROCESS_MEMORY_COUNTERS_EX pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
#endif
    return 0.0;
}

// TODO multiple output streams
// TODO multiple output files
// TODO run video filter, video encoder, audio filter, audio encoder in separate threads.
// TODO realtime with smaller buffer?

struct Stream
{
    std::shared_ptr<AVFilterGraph> graph  = nullptr;
    AVFilterContext*               sink   = nullptr;
    AVFilterContext*               source = nullptr;

    std::shared_ptr<AVCodecContext> enc = nullptr;
    AVStream*                       st  = nullptr;

    tbb::concurrent_bounded_queue<std::shared_ptr<SwsContext>> sws_;

    int64_t pts = 0;
    int audio_channel_index = -1;  // XDCAM HD422: índice do canal mono (-1 = todos os canais)
    
    // INTERLACED ACCUMULATION: Para formatos interlaced, acumulamos 2 callbacks (campos) em 1 frame
    int field_accumulator_count = 0;           // contador de campos acumulados (0 ou 1)
    std::vector<int32_t> audio_accumulator;    // buffer para acumular samples de áudio de 2 campos
    bool is_interlaced_format = false;         // true se field_count > 1
    
    // DEBUG: variáveis para rastreamento de sincronização
    int64_t debug_frame_count = 0;
    std::chrono::high_resolution_clock::time_point debug_last_callback_time;
    std::chrono::high_resolution_clock::time_point debug_start_time;
    int64_t debug_total_samples = 0;  // acumulador de samples de áudio
    
    // TIMING: medição de tempo de cada etapa do encoding (em microsegundos)
    int64_t timing_sws_scale_us = 0;      // tempo de conversão BGRA->YUV422P
    int64_t timing_send_frame_us = 0;     // tempo de avcodec_send_frame
    int64_t timing_receive_packet_us = 0; // tempo de avcodec_receive_packet
    int64_t timing_frame_total_us = 0;    // tempo total do frame
    
    // MEMORY: rastreamento de uso de memória para detectar leaks
    double memory_initial_mb = 0.0;       // memória no início do encoding
    
    // DIAG: contadores para diagnóstico de backlog do encoder
    int64_t diag_frames_sent = 0;       // frames enviados ao encoder (avcodec_send_frame)
    int64_t diag_packets_received = 0;  // pacotes recebidos do encoder (avcodec_receive_packet)
    int64_t diag_last_log_frame = 0;                // último frame onde fizemos log
    int64_t diag_callback_min_us = INT64_MAX;       // menor intervalo de callback (detectar rajadas)
    int64_t diag_callback_max_us = 0;               // maior intervalo de callback (detectar atrasos)
    double diag_last_memory_mb = 0.0;               // última memória logada

    Stream(AVFormatContext*                    oc,
           std::string                         suffix,
           AVCodecID                           codec_id,
           const core::video_format_desc&      format_desc,
           bool                                realtime,
           std::map<std::string, std::string>& options,
           int                                 channel_index = -1)  // XDCAM HD422: canal mono
        : audio_channel_index(channel_index)
        , is_interlaced_format(format_desc.field_count > 1)
    {
        std::map<std::string, std::string> stream_options;

        {
            auto tmp = std::move(options);
            for (auto& p : tmp) {
                if (boost::algorithm::ends_with(p.first, suffix)) {
                    const auto key = p.first.substr(0, p.first.size() - suffix.size());
                    stream_options.emplace(key, std::move(p.second));
                } else {
                    options.insert(std::move(p));
                }
            }
        }

        std::string filter_spec = "";
        {
            const auto it = stream_options.find("filter");
            if (it != stream_options.end()) {
                filter_spec = std::move(it->second);
                stream_options.erase(it);
            }
        }

        auto codec = avcodec_find_encoder(codec_id);
        {
            const auto it = stream_options.find("codec");
            if (it != stream_options.end()) {
                codec = avcodec_find_encoder_by_name(it->second.c_str());
                stream_options.erase(it);
            }
        }

        if (!codec) {
            FF_RET(AVERROR(EINVAL), "avcodec_find_encoder");
        }

        AVFilterInOut* outputs = nullptr;
        AVFilterInOut* inputs  = nullptr;

        CASPAR_SCOPE_EXIT
        {
            avfilter_inout_free(&inputs);
            avfilter_inout_free(&outputs);
        };

        graph = std::shared_ptr<AVFilterGraph>(avfilter_graph_alloc(),
                                               [](AVFilterGraph* ptr) { avfilter_graph_free(&ptr); });

        if (!graph) {
            FF_RET(AVERROR(ENOMEM), "avfilter_graph_alloc");
        }

        graph->nb_threads = 16;
        graph->execute    = graph_execute;

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            if (filter_spec.empty()) {
                filter_spec = "null";
            }
        } else {
            if (filter_spec.empty()) {
                filter_spec = "anull";
            }
        }

        FF(avfilter_graph_parse2(graph.get(), filter_spec.c_str(), &inputs, &outputs));

        {
            auto cur = inputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph input count"));
            }

            if (codec->type == AVMEDIA_TYPE_VIDEO) {
                const auto sar = boost::rational<int>(format_desc.square_width, format_desc.square_height) /
                                 boost::rational<int>(format_desc.width, format_desc.height);

                // XDCAM HD422: Calcular frame rate real
                // Para interlaced: format_desc.framerate é field rate (ex: 60000/1001 para 1080i5994)
                // Precisamos do frame rate (metade): 30000/1001 para 1080i5994
                // Para progressivo: format_desc.framerate já é frame rate
                int frame_rate_num = format_desc.framerate.numerator();
                int frame_rate_den = format_desc.framerate.denominator();
                
                // Detectar interlaced: field_count > 1 indica interlaced
                if (format_desc.field_count > 1) {
                    // Interlaced: dividir numerador por 2 (field rate -> frame rate)
                    // 60000/1001 -> 30000/1001 (forma canônica para NTSC)
                    frame_rate_num /= 2;
                    CASPAR_LOG(info) << "[ENCODER] Interlaced format detected - using frame rate: " 
                        << frame_rate_num << "/" << frame_rate_den 
                        << " (" << std::fixed << std::setprecision(2) << (double)frame_rate_num / frame_rate_den << " fps)"
                        << " - Acumulando 2 campos por frame";
                } else {
                    CASPAR_LOG(info) << "[ENCODER] Progressive format - using frame rate: "
                        << frame_rate_num << "/" << frame_rate_den
                        << " (" << std::fixed << std::setprecision(2) << (double)frame_rate_num / frame_rate_den << " fps)";
                }
                
                auto args = (boost::format("video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:sar=%d/%d:frame_rate=%d/%d") %
                             format_desc.width % format_desc.height % AV_PIX_FMT_YUV422P % 
                             frame_rate_den %  // time_base = 1/frame_rate
                             frame_rate_num % sar.numerator() % sar.denominator() %
                             frame_rate_num % 
                             frame_rate_den)
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("buffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
            } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
                // XDCAM HD422: mono (1 canal) por stream, 48kHz
                auto args = (boost::format("time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=%#x") % 1 %
                             48000 % 48000 % av_get_sample_fmt_name(AV_SAMPLE_FMT_S32) %
                             AV_CH_LAYOUT_MONO)
                                .str();
                auto name = (boost::format("in_%d") % 0).str();

                FF(avfilter_graph_create_filter(
                    &source, avfilter_get_by_name("abuffer"), name.c_str(), args.c_str(), nullptr, graph.get()));
                FF(avfilter_link(source, 0, cur->filter_ctx, cur->pad_idx));
            } else {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter input media type"));
            }
        }

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph.get()));

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            // TODO codec->profiles
            // TODO FF(av_opt_set_int_list(sink, "framerates", codec->supported_framerates, { 0, 0 },
            // AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "pix_fmts", codec->pix_fmts, -1, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
            FF(avfilter_graph_create_filter(
                &sink, avfilter_get_by_name("abuffersink"), "out", nullptr, nullptr, graph.get()));
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4245)
#endif
            // TODO codec->profiles
            FF(av_opt_set_int_list(sink, "sample_fmts", codec->sample_fmts, -1, AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "channel_layouts", codec->channel_layouts, 0, AV_OPT_SEARCH_CHILDREN));
            FF(av_opt_set_int_list(sink, "sample_rates", codec->supported_samplerates, 0, AV_OPT_SEARCH_CHILDREN));
#ifdef _MSC_VER
#pragma warning(pop)
#endif
        } else {
            CASPAR_THROW_EXCEPTION(ffmpeg_error_t()
                                   << boost::errinfo_errno(EINVAL) << msg_info_t("invalid output media type"));
        }

        {
            const auto cur = outputs;

            if (!cur || cur->next) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter graph output count"));
            }

            if (avfilter_pad_get_type(cur->filter_ctx->output_pads, cur->pad_idx) != codec->type) {
                CASPAR_THROW_EXCEPTION(ffmpeg_error_t() << boost::errinfo_errno(EINVAL)
                                                        << msg_info_t("invalid filter output media type"));
            }

            FF(avfilter_link(cur->filter_ctx, cur->pad_idx, sink, 0));
        }

        FF(avfilter_graph_config(graph.get(), nullptr));

        st = avformat_new_stream(oc, nullptr);
        if (!st) {
            FF_RET(AVERROR(ENOMEM), "avformat_new_stream");
        }

        enc = std::shared_ptr<AVCodecContext>(avcodec_alloc_context3(codec),
                                              [](AVCodecContext* ptr) { avcodec_free_context(&ptr); });

        if (!enc) {
            FF_RET(AVERROR(ENOMEM), "avcodec_alloc_context3")
        }

        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            // XDCAM HD422: Calcular frame rate real (não field rate)
            // Para interlaced: format_desc.framerate é field rate (ex: 60000/1001 para 1080i5994)
            // Precisamos do frame rate (metade): 30000/1001 para 1080i5994
            int frame_rate_num = format_desc.framerate.numerator();
            int frame_rate_den = format_desc.framerate.denominator();
            
            if (format_desc.field_count > 1) {
                // Interlaced: dividir NUMERADOR por 2 (field rate -> frame rate)
                // 60000/1001 -> 30000/1001 (forma canônica para NTSC)
                frame_rate_num /= 2;
            }
            
            AVRational frame_rate = { frame_rate_num, frame_rate_den };
            st->time_base = av_inv_q(frame_rate);  // time_base = 1001/30000 para 29.97fps
            st->avg_frame_rate = frame_rate;  // Necessário para trilha de timecode (write_tmcd)

            enc->width               = av_buffersink_get_w(sink);
            enc->height              = av_buffersink_get_h(sink);
            enc->framerate           = frame_rate;
            enc->sample_aspect_ratio = av_buffersink_get_sample_aspect_ratio(sink);
            enc->time_base           = st->time_base;
            enc->pix_fmt             = static_cast<AVPixelFormat>(av_buffersink_get_format(sink));
            
            // XDCAM HD422: setar field_order e codec_tag diretamente
            // Mantemos interlaced TFF mesmo com frame rate (encoder sabe que são 2 campos por frame)
            enc->field_order         = AV_FIELD_TT;  // Top Field First (metadados)
            enc->codec_tag           = MKTAG('x', 'd', '5', 'b');  // XDCAM HD422
            av_opt_set_int(enc->priv_data, "top", 1, 0);  // Top Field First (encoder MPEG-2)
            
            // XDCAM HD422: Metadados de cor BT.709 (como arquivo de referência)
            enc->color_primaries     = AVCOL_PRI_BT709;
            enc->color_trc           = AVCOL_TRC_BT709;
            enc->colorspace          = AVCOL_SPC_BT709;
        } else if (codec->type == AVMEDIA_TYPE_AUDIO) {
            st->time_base = {1, av_buffersink_get_sample_rate(sink)};

            enc->sample_fmt     = static_cast<AVSampleFormat>(av_buffersink_get_format(sink));
            enc->sample_rate    = av_buffersink_get_sample_rate(sink);
            enc->channels       = av_buffersink_get_channels(sink);
            enc->channel_layout = av_buffersink_get_channel_layout(sink);
            enc->time_base      = st->time_base;
            
            // XDCAM HD422: Usar codec tag padrão 'in24' do FFmpeg para PCM 24-bit
            // (tag 'lpcm' é incompatível com codec PCM_S24LE no muxer MOV)

            if (!enc->channels) {
                enc->channels = av_get_channel_layout_nb_channels(enc->channel_layout);
            } else if (!enc->channel_layout) {
                enc->channel_layout = av_get_default_channel_layout(enc->channels);
            }
        } else {
            // TODO
        }

        // Threading para acelerar encoding
        // MPEG-2 suporta slice threading (cada thread processa fatias horizontais)
        if (codec->type == AVMEDIA_TYPE_VIDEO) {
            enc->thread_count = 0;  // 0 = auto-detectar número de cores
            if (codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
                enc->thread_type = FF_THREAD_SLICE;
                CASPAR_LOG(info) << "[ENCODER] Video encoder using SLICE threading (parallel horizontal slices)";
            } else if (codec->capabilities & AV_CODEC_CAP_FRAME_THREADS) {
                enc->thread_type = FF_THREAD_FRAME;
                CASPAR_LOG(info) << "[ENCODER] Video encoder using FRAME threading";
            } else {
                CASPAR_LOG(warning) << "[ENCODER] Video encoder has NO threading support - may be slow!";
            }
        } else if (realtime && codec->capabilities & AV_CODEC_CAP_SLICE_THREADS) {
            enc->thread_type = FF_THREAD_SLICE;
        }

        auto dict = to_dict(std::move(stream_options));
        CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
        FF(avcodec_open2(enc.get(), codec, &dict));
        for (auto& p : to_map(&dict)) {
            options[p.first] = p.second + suffix;
        }

        FF(avcodec_parameters_from_context(st->codecpar, enc.get()));

        if (codec->type == AVMEDIA_TYPE_AUDIO && !(codec->capabilities & AV_CODEC_CAP_VARIABLE_FRAME_SIZE)) {
            av_buffersink_set_frame_size(sink, enc->frame_size);
        }

        if (oc->oformat->flags & AVFMT_GLOBALHEADER) {
            enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
        }
    }

    std::shared_ptr<SwsContext> get_sws(int width, int height)
    {
        std::shared_ptr<SwsContext> sws;

        if (sws_.try_pop(sws)) {
            return sws;
        }

        // XDCAM HD422: YUV422P sem canal alpha
        sws.reset(sws_getContext(
                      width, height, AV_PIX_FMT_BGRA, width, height, AV_PIX_FMT_YUV422P, 0, nullptr, nullptr, nullptr),
                  [](SwsContext* ptr) { sws_freeContext(ptr); });

        if (!sws) {
            CASPAR_THROW_EXCEPTION(caspar_exception());
        }

        int        brigthness;
        int        contrast;
        int        saturation;
        int        in_full;
        int        out_full;
        const int* inv_table;
        const int* table;

        sws_getColorspaceDetails(
            sws.get(), (int**)&inv_table, &in_full, (int**)&table, &out_full, &brigthness, &contrast, &saturation);

        inv_table = sws_getCoefficients(AVCOL_SPC_RGB);
        table     = sws_getCoefficients(AVCOL_SPC_BT709);

        in_full  = AVCOL_RANGE_JPEG;
        out_full = AVCOL_RANGE_MPEG;

        sws_setColorspaceDetails(sws.get(), inv_table, in_full, table, out_full, brigthness, contrast, saturation);

        return std::shared_ptr<SwsContext>(sws.get(), [this, sws](SwsContext*) { sws_.push(sws); });
    }

    void send(core::const_frame&                             in_frame,
              const core::video_format_desc&                 format_desc,
              std::function<void(std::shared_ptr<AVPacket>)> cb)
    {
        std::shared_ptr<AVFrame>  frame;
        std::shared_ptr<AVPacket> pkt;

        if (in_frame) {
            // DEBUG: calcular intervalo entre callbacks
            auto now = std::chrono::high_resolution_clock::now();
            if (debug_frame_count == 0) {
                debug_start_time = now;
                debug_last_callback_time = now;
            }
            
            auto callback_interval_us = std::chrono::duration_cast<std::chrono::microseconds>(
                now - debug_last_callback_time).count();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - debug_start_time).count();
            debug_last_callback_time = now;
            
            // DIAG: rastrear min/max de intervalo de callback (apenas para vídeo)
            if (enc->codec_type == AVMEDIA_TYPE_VIDEO && debug_frame_count > 0) {
                if (callback_interval_us < diag_callback_min_us) diag_callback_min_us = callback_interval_us;
                if (callback_interval_us > diag_callback_max_us) diag_callback_max_us = callback_interval_us;
            }
            
            if (enc->codec_type == AVMEDIA_TYPE_VIDEO) {
                // INTERLACED ACCUMULATION: Para interlaced, processamos apenas a cada 2 callbacks
                // O CasparCG chama 60x/segundo para 1080i5994, mas queremos encodar 30 frames/segundo
                // Cada frame já contém 2 campos entrelaçados, então pulamos callbacks alternados
                
                if (is_interlaced_format) {
                    field_accumulator_count++;
                    
                    // Pular callback ímpar (1, 3, 5...) - processar apenas no par (2, 4, 6...)
                    if (field_accumulator_count % 2 == 1) {
                        // Primeiro campo do par - apenas retornar sem processar
                        // O vídeo será processado no próximo callback
                        return;
                    }
                    // Segundo campo do par - processar o frame completo
                }
                
                auto frame_start_time = std::chrono::high_resolution_clock::now();
                
                frame = make_av_video_frame(in_frame, format_desc);

                {
                    auto frame2                 = alloc_frame();
                    frame2->sample_aspect_ratio = frame->sample_aspect_ratio;
                    frame2->width               = frame->width;
                    frame2->height              = frame->height;
                    // XDCAM HD422: YUV422P sem canal alpha
                    frame2->format              = AV_PIX_FMT_YUV422P;
                    frame2->colorspace          = AVCOL_SPC_BT709;
                    frame2->color_primaries     = AVCOL_PRI_BT709;
                    frame2->color_range         = AVCOL_RANGE_MPEG;
                    frame2->color_trc           = AVCOL_TRC_BT709;
                    // XDCAM HD422: Top Field First interlaced
                    frame2->interlaced_frame    = 1;
                    frame2->top_field_first     = 1;
                    av_frame_get_buffer(frame2.get(), 64);

                    auto sws_start = std::chrono::high_resolution_clock::now();
                    int h = frame->height / 8;
                    tbb::parallel_for(0, 8, [&](int i) {
                        auto sws = get_sws(frame->width, h);

                        uint8_t* src[4] = {};
                        src[0]          = frame->data[0] + frame->linesize[0] * (i * h);

                        uint8_t* dst[4] = {};
                        dst[0]          = frame2->data[0] + frame2->linesize[0] * (i * h);
                        dst[1]          = frame2->data[1] + frame2->linesize[1] * (i * h);
                        dst[2]          = frame2->data[2] + frame2->linesize[2] * (i * h);

                        sws_scale(sws.get(), src, frame->linesize, 0, h, dst, frame2->linesize);
                    });
                    timing_sws_scale_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - sws_start).count();

                    int i = frame->height - h;
                    if (i > 0) {
                        // TODO
                    }

                    frame = std::move(frame2);
                }

                frame->pts = pts;
                
                // Calcular tempo total do frame até aqui
                timing_frame_total_us = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::high_resolution_clock::now() - frame_start_time).count();
                
                // DEBUG: log apenas a cada 300 frames (~10 segundos) OU quando há problema
                int fr_num = format_desc.framerate.numerator();
                int fr_den = format_desc.framerate.denominator();
                if (format_desc.field_count > 1) {
                    fr_num /= 2;
                }
                double max_frame_time_us = 1000000.0 * fr_den / fr_num;
                bool is_slow = timing_frame_total_us > max_frame_time_us;
                
                // Capturar memória (usado para detecção de leak)
                double current_memory_mb = get_process_memory_mb();
                if (debug_frame_count == 0) {
                    memory_initial_mb = current_memory_mb;
                }
                double memory_delta_mb = current_memory_mb - memory_initial_mb;
                bool has_memory_issue = memory_delta_mb > 200;
                
                // DIAG: calcular backlog do encoder (frames pendentes dentro do codec)
                int64_t encoder_backlog = diag_frames_sent - diag_packets_received;
                bool has_encoder_backlog = encoder_backlog > 5;
                
                // DIAG: detectar rajada de callbacks (intervalo < 10ms) ou atraso (> 50ms)
                bool has_callback_anomaly = (diag_callback_min_us < 10000 || diag_callback_max_us > 50000) 
                                            && debug_frame_count > 10;
                
                // Log apenas a cada 300 frames OU se há qualquer anomalia
                bool should_log = (debug_frame_count % 300 == 0) 
                                || is_slow || has_memory_issue 
                                || has_encoder_backlog || has_callback_anomaly;
                
                // Throttle: mínimo 150 frames (~5s) entre logs de anomalia
                bool throttle_ok = (debug_frame_count - diag_last_log_frame) >= 150;
                
                if (should_log && throttle_ok) {
                    double expected_time_ms = (double)debug_frame_count * 1000.0 * fr_den / fr_num;
                    double drift_ms = elapsed_ms - expected_time_ms;
                    
                    CASPAR_LOG(debug) << "[DIAG] f=" << debug_frame_count 
                        << " enc=" << timing_send_frame_us << "us"
                        << " backlog=" << encoder_backlog
                        << " cb_min=" << diag_callback_min_us << "us"
                        << " cb_max=" << diag_callback_max_us << "us"
                        << " drift=" << std::fixed << std::setprecision(0) << drift_ms << "ms"
                        << " mem=" << std::fixed << std::setprecision(0) << memory_delta_mb << "MB"
                        << (is_slow ? " [SLOW]" : "")
                        << (has_memory_issue ? " [MEM!]" : "")
                        << (has_encoder_backlog ? " [BACKLOG!]" : "")
                        << (has_callback_anomaly ? " [CB-ANOMALY]" : "");
                    
                    diag_last_log_frame = debug_frame_count;
                    // Resetar min/max para próxima janela
                    diag_callback_min_us = INT64_MAX;
                    diag_callback_max_us = 0;
                }
                
                debug_frame_count++;
                pts += 1;
                FF(av_buffersrc_write_frame(source, frame.get()));
            } else if (enc->codec_type == AVMEDIA_TYPE_AUDIO) {
                // XDCAM HD422: extrair apenas o canal mono especificado
                frame = make_av_audio_frame(in_frame, format_desc, audio_channel_index);
                
                // INTERLACED ACCUMULATION: Para interlaced, acumulamos 2 callbacks de áudio
                if (is_interlaced_format) {
                    // Copiar samples para o acumulador
                    const int32_t* src_data = reinterpret_cast<const int32_t*>(frame->data[0]);
                    int nb_samples = frame->nb_samples;
                    
                    // Adicionar ao buffer de acumulação
                    size_t old_size = audio_accumulator.size();
                    audio_accumulator.resize(old_size + nb_samples);
                    std::memcpy(audio_accumulator.data() + old_size, src_data, nb_samples * sizeof(int32_t));
                    
                    field_accumulator_count++;
                    
                    // Se ainda não temos 2 callbacks, apenas retornar
                    if (field_accumulator_count % 2 == 1) {
                        return;
                    }
                    
                    // Temos 2 callbacks acumulados - criar frame com samples combinados
                    int total_samples = static_cast<int>(audio_accumulator.size());
                    
                    auto combined_frame = alloc_frame();
                    combined_frame->format = frame->format;
                    combined_frame->sample_rate = frame->sample_rate;
                    combined_frame->channels = frame->channels;
                    combined_frame->channel_layout = frame->channel_layout;
                    combined_frame->nb_samples = total_samples;
                    av_frame_get_buffer(combined_frame.get(), 0);
                    
                    // Copiar samples acumulados para o novo frame
                    std::memcpy(combined_frame->data[0], audio_accumulator.data(), total_samples * sizeof(int32_t));
                    
                    // Limpar o acumulador
                    audio_accumulator.clear();
                    
                    // Usar o frame combinado
                    frame = std::move(combined_frame);
                }
                
                frame->pts = pts;
                
                // DEBUG: log de áudio (apenas para canal 0, para não poluir)
                if (audio_channel_index == 0) {
                    debug_total_samples += frame->nb_samples;
                    
                    if (debug_frame_count < 10 || debug_frame_count % 30 == 0) {
                        // Calcular frame rate real (encoder está a 30fps para interlaced)
                        int fr_num = format_desc.framerate.numerator();
                        int fr_den = format_desc.framerate.denominator();
                        if (format_desc.field_count > 1) {
                            fr_num /= 2;  // Interlaced: field rate -> frame rate (30000/1001)
                        }
                        
                        double expected_time_ms = (double)debug_frame_count * 1000.0 * fr_den / fr_num;
                        double audio_time_ms = (double)debug_total_samples * 1000.0 / 48000.0;
                        double av_drift_ms = audio_time_ms - expected_time_ms;
                        
                        CASPAR_LOG(debug) << "[SYNC-DEBUG] AUDIO ch=" << audio_channel_index
                            << " frame=" << debug_frame_count
                            << " pts=" << pts
                            << " nb_samples=" << frame->nb_samples
                            << " total_samples=" << debug_total_samples
                            << " audio_time_ms=" << std::fixed << std::setprecision(1) << audio_time_ms
                            << " expected_ms=" << std::fixed << std::setprecision(1) << expected_time_ms
                            << " av_drift_ms=" << std::fixed << std::setprecision(2) << av_drift_ms
                            << " callback_interval_us=" << callback_interval_us
                            << (is_interlaced_format ? " [INTERLACED-ACCUM]" : "");
                    }
                    debug_frame_count++;
                }
                
                pts += frame->nb_samples;
                FF(av_buffersrc_write_frame(source, frame.get()));
            } else {
                // TODO
            }
        } else {
            // DEBUG: log final
            CASPAR_LOG(info) << "[SYNC-DEBUG] STREAM CLOSED - total_frames=" << debug_frame_count
                << " total_samples=" << debug_total_samples
                << " final_pts=" << pts;
            FF(av_buffersrc_close(source, pts, 0));
        }

        while (true) {
            auto receive_start = std::chrono::high_resolution_clock::now();
            pkt     = alloc_packet();
            int ret = avcodec_receive_packet(enc.get(), pkt.get());
            timing_receive_packet_us = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::high_resolution_clock::now() - receive_start).count();

            if (ret == AVERROR(EAGAIN)) {
                frame = alloc_frame();
                ret   = av_buffersink_get_frame(sink, frame.get());
                if (ret == AVERROR(EAGAIN)) {
                    return;
                }
                if (ret == AVERROR_EOF) {
                    auto send_start = std::chrono::high_resolution_clock::now();
                    FF(avcodec_send_frame(enc.get(), nullptr));
                    timing_send_frame_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - send_start).count();
                } else {
                    FF_RET(ret, "av_buffersink_get_frame");
                    auto send_start = std::chrono::high_resolution_clock::now();
                    FF(avcodec_send_frame(enc.get(), frame.get()));
                    timing_send_frame_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - send_start).count();
                    diag_frames_sent++;  // DIAG: incrementar contador de frames enviados
                }
            } else if (ret == AVERROR_EOF) {
                return;
            } else {
                FF_RET(ret, "avcodec_receive_packet");
                diag_packets_received++;  // DIAG: incrementar contador de pacotes recebidos
                pkt->stream_index = st->index;
                av_packet_rescale_ts(pkt.get(), enc->time_base, st->time_base);
                cb(std::move(pkt));
            }
        }
    }
};

struct ffmpeg_consumer : public core::frame_consumer
{
    core::monitor::state    state_;
    mutable std::mutex      state_mutex_;
    int                     channel_index_ = -1;
    core::video_format_desc format_desc_;
    bool                    realtime_ = false;

    spl::shared_ptr<diagnostics::graph> graph_;

    std::string path_;
    std::string args_;

    std::exception_ptr exception_;
    std::mutex         exception_mutex_;

    tbb::concurrent_bounded_queue<core::const_frame> frame_buffer_;
    std::thread                                      frame_thread_;

  public:
    ffmpeg_consumer(std::string path, std::string args, bool realtime)
        : channel_index_([&] {
            boost::crc_16_type result;
            result.process_bytes(path.data(), path.length());
            return result.checksum();
        }())
        , realtime_(realtime)
        , path_(std::move(path))
        , args_(std::move(args))
    {
        state_["file/path"] = u8(path_);

        frame_buffer_.set_capacity(realtime_ ? 1 : 256);  // Aumentado de 64 para 256 (~4s buffer)

        diagnostics::register_graph(graph_);
        graph_->set_color("frame-time", diagnostics::color(0.1f, 1.0f, 0.1f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("input", diagnostics::color(0.7f, 0.4f, 0.4f));
    }

    ~ffmpeg_consumer()
    {
        if (frame_thread_.joinable()) {
            frame_buffer_.push(core::const_frame{});
            frame_thread_.join();
        }
    }

    // frame consumer

    void initialize(const core::video_format_desc& format_desc, int channel_index) override
    {
        if (frame_thread_.joinable()) {
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Cannot reinitialize ffmpeg-consumer."));
        }

        format_desc_   = format_desc;
        channel_index_ = channel_index;

        graph_->set_text(print());

        frame_thread_ = std::thread([=] {
            try {
                std::map<std::string, std::string> options;
                {
                    static boost::regex opt_exp("-(?<NAME>[^-\\s]+)(\\s+(?<VALUE>[^\\s]+))?");
                    for (auto it = boost::sregex_iterator(args_.begin(), args_.end(), opt_exp);
                         it != boost::sregex_iterator();
                         ++it) {
                        options[(*it)["NAME"].str().c_str()] =
                            (*it)["VALUE"].matched ? (*it)["VALUE"].str().c_str() : "";
                    }
                }

                boost::filesystem::path full_path = path_;

                // XDCAM HD422: Forçar extensão .mov se não estiver presente
                if (!boost::iequals(full_path.extension().string(), ".mov")) {
                    full_path.replace_extension(".mov");
                }

                static boost::regex prot_exp("^.+:.*");
                if (!boost::regex_match(full_path.string(), prot_exp)) {
                    if (!full_path.is_complete()) {
                        full_path = u8(env::media_folder()) / full_path;
                    }

                    // TODO -y?
                    if (boost::filesystem::exists(full_path)) {
                        boost::filesystem::remove(full_path);
                    }

                    boost::filesystem::create_directories(full_path.parent_path());
                }

                AVFormatContext* oc = nullptr;

                // XDCAM HD422: Forçar container MOV (QuickTime tradicional)
                FF(avformat_alloc_output_context2(&oc, nullptr, "mov", full_path.string().c_str()));
                
                // XDCAM HD422: Configurar MOV para QuickTime tradicional (não MPEG-4/ISO)
                // brand=qt força o formato QuickTime clássico
                // write_tmcd=1 cria trilha de timecode QuickTime TC
                int brand_ret = av_opt_set(oc->priv_data, "brand", "qt  ", 0);
                int tmcd_ret = av_opt_set_int(oc->priv_data, "write_tmcd", 1, 0);
                
                // XDCAM HD422: Configurar timescale do vídeo para trilha de timecode
                // Usa frame rate real (não field rate)
                int frame_rate_num = format_desc.framerate.numerator();
                if (format_desc.field_count > 1) {
                    // Para interlaced, usar frame rate (metade do field rate)
                    // Ex: 1080i5994 -> 30000 (não 60000)
                    frame_rate_num = format_desc.framerate.numerator();  // mantém o mesmo numerador
                }
                int video_timescale = frame_rate_num;
                av_opt_set_int(oc->priv_data, "video_track_timescale", video_timescale, 0);
                
                if (brand_ret < 0) {
                    CASPAR_LOG(warning) << "XDCAM HD422: Não foi possível setar brand=qt via av_opt_set, usando fallback via dict";
                }
                if (tmcd_ret < 0) {
                    CASPAR_LOG(warning) << "XDCAM HD422: Não foi possível setar write_tmcd=1 via av_opt_set, usando fallback via dict";
                }

                CASPAR_SCOPE_EXIT { avformat_free_context(oc); };

                boost::optional<Stream> video_stream;
                if (oc->oformat->video_codec != AV_CODEC_ID_NONE) {
                    // XDCAM HD422: MPEG-2 4:2:2 50Mbps CBR interlaced
                    options["b:v"] = "50000000";
                    options["minrate:v"] = "50000000";
                    options["maxrate:v"] = "50000000";
                    options["bufsize:v"] = "17825792";
                    options["g:v"] = "12";  // GOP N=12 (reduzido de 15 para melhor performance)
                    options["bf:v"] = "2";  // M=3 (XDCAM Long GOP spec)
                    options["flags:v"] = "+ildct+ilme";
                    options["dc:v"] = "10";
                    options["qmin:v"] = "1";
                    options["qmax:v"] = "12";
                    // STRICT GOP: Desabilitar scene change detection e forçar GOP estrito
                    // Isso evita que o encoder quebre GOPs aleatoriamente e reduz processamento
                    options["sc_threshold:v"] = "1000000000";  // Desabilita scene change detection
                    options["b_strategy:v"] = "0";  // Desabilita decisão adaptativa de B-frames
                    // VBV CONTROL: Forçar uso estrito do buffer VBV para CBR consistente
                    options["rc_max_vbv_use:v"] = "1";
                    options["rc_min_vbv_use:v"] = "1";
                    // MPEG-2 4:2:2 50Mbps - usando matrizes padrão para melhor performance
                    // field_order e codec_tag setados diretamente no AVCodecContext
                    video_stream.emplace(oc, ":v", AV_CODEC_ID_MPEG2VIDEO, format_desc, realtime_, options);

                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        // Usar frame rate real (considerando interlaced)
                        double fps = static_cast<double>(format_desc.framerate.numerator()) / 
                                     static_cast<double>(format_desc.framerate.denominator());
                        if (format_desc.field_count > 1) {
                            fps /= 2.0;  // Interlaced: field rate -> frame rate
                        }
                        state_["file/fps"] = fps;
                    }
                }

                // 4 streams de áudio mono separados (L, R, C, LFE)
                std::vector<boost::optional<Stream>> audio_streams;
                if (oc->oformat->audio_codec != AV_CODEC_ID_NONE) {
                    for (int ch = 0; ch < 4; ++ch) {
                        std::string suffix = ":a" + std::to_string(ch);
                        options["ar" + suffix] = "48000";
                        audio_streams.emplace_back();
                        audio_streams.back().emplace(oc, suffix, AV_CODEC_ID_PCM_S24LE, format_desc, realtime_, options, ch);
                    }
                }

                if (!(oc->oformat->flags & AVFMT_NOFILE)) {
                    // TODO (fix) interrupt_cb
                    auto dict = to_dict(std::move(options));
                    CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
                    FF(avio_open2(&oc->pb, full_path.string().c_str(), AVIO_FLAG_WRITE, nullptr, &dict));
                    options = to_map(&dict);
                }

                // XDCAM HD422: Gerar timecode baseado na hora atual do sistema
                {
                    auto now = std::chrono::system_clock::now();
                    std::time_t now_t = std::chrono::system_clock::to_time_t(now);
                    std::tm* now_tm = std::localtime(&now_t);
                    
                    // Calcular frames a partir dos milissegundos
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        now.time_since_epoch()) % 1000;
                    
                    // Detectar drop-frame: 29.97fps (30000/1001), 59.94fps (60000/1001), etc.
                    double fps = format_desc.fps;
                    bool drop_frame = false;
                    int nominal_fps = 30;  // fps nominal para cálculo de frames
                    
                    // 29.97fps (NTSC)
                    if (fps > 29.9 && fps < 30.0) {
                        drop_frame = true;
                        nominal_fps = 30;
                    }
                    // 59.94fps (NTSC progressive)
                    else if (fps > 59.8 && fps < 60.0) {
                        drop_frame = true;
                        nominal_fps = 60;
                    }
                    // 23.976fps (film)
                    else if (fps > 23.9 && fps < 24.0) {
                        drop_frame = false;  // 23.976 não usa drop-frame
                        nominal_fps = 24;
                    }
                    // Outros framerates (25, 50, 30, 60, etc.)
                    else {
                        nominal_fps = static_cast<int>(fps + 0.5);
                    }
                    
                    // Calcular frame a partir dos milissegundos
                    int frame = static_cast<int>((ms.count() * nominal_fps) / 1000);
                    if (frame >= nominal_fps) frame = nominal_fps - 1;  // Garantir frame válido
                    
                    // Formatar timecode: HH:MM:SS:FF (non-drop) ou HH:MM:SS;FF (drop-frame)
                    char tc_str[16];
                    char separator = drop_frame ? ';' : ':';
                    snprintf(tc_str, sizeof(tc_str), "%02d:%02d:%02d%c%02d",
                             now_tm->tm_hour, now_tm->tm_min, now_tm->tm_sec, separator, frame);
                    
                    av_dict_set(&oc->metadata, "timecode", tc_str, 0);
                    
                    CASPAR_LOG(info) << print() << " Timecode inicial: " << tc_str;
                }

                {
                    // XDCAM HD422: Adicionar opções de muxer MOV como fallback via dicionário
                    // Estas opções garantem formato QuickTime tradicional e trilha de timecode
                    options["brand"] = "qt  ";
                    options["write_tmcd"] = "1";
                    
                    auto dict = to_dict(std::move(options));
                    CASPAR_SCOPE_EXIT { av_dict_free(&dict); };
                    FF(avformat_write_header(oc, &dict));
                    options = to_map(&dict);
                }

                {
                    for (auto& p : options) {
                        CASPAR_LOG(warning) << print() << " Unused option " << p.first << "=" << p.second;
                    }
                }

                tbb::concurrent_bounded_queue<std::shared_ptr<AVPacket>> packet_buffer;
                packet_buffer.set_capacity(realtime_ ? 1 : 512);  // Aumentado de 128 para 512
                
                // TIMING: contadores para thread de escrita
                std::atomic<int64_t> write_total_packets{0};
                std::atomic<int64_t> write_total_time_us{0};
                std::atomic<int64_t> write_max_time_us{0};
                std::atomic<int64_t> write_queue_size{0};
                
                auto packet_thread = std::thread([&] {
                    try {
                        CASPAR_SCOPE_EXIT
                        {
                            if (!(oc->oformat->flags & AVFMT_NOFILE)) {
                                FF(avio_closep(&oc->pb));
                            }
                        };

                        std::map<int, int64_t> count;

                        std::shared_ptr<AVPacket> pkt;
                        while (true) {
                            packet_buffer.pop(pkt);
                            if (!pkt) {
                                break;
                            }
                            count[pkt->stream_index] += 1;
                            
                            // TIMING: medir tempo de escrita
                            auto write_start = std::chrono::high_resolution_clock::now();
                            FF(av_interleaved_write_frame(oc, pkt.get()));
                            auto write_time = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::high_resolution_clock::now() - write_start).count();
                            
                            write_total_packets++;
                            write_total_time_us += write_time;
                            if (write_time > write_max_time_us) {
                                write_max_time_us = write_time;
                            }
                            write_queue_size = packet_buffer.size();
                        }

                        auto video_st = video_stream ? video_stream->st : nullptr;
                        
                        // XDCAM HD422: verificar todos os 8 streams de áudio
                        bool all_audio_ok = true;
                        for (const auto& audio_stream : audio_streams) {
                            if (audio_stream && !count[audio_stream->st->index]) {
                                all_audio_ok = false;
                                break;
                            }
                        }

                        if ((!video_st || count[video_st->index]) && all_audio_ok) {
                            FF(av_write_trailer(oc));
                        }

                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                        // TODO
                        packet_buffer.abort();
                    }
                });
                CASPAR_SCOPE_EXIT
                {
                    if (packet_thread.joinable()) {
                        // TODO Is nullptr needed?
                        packet_buffer.push(nullptr);
                        packet_buffer.abort();
                        packet_thread.join();
                    }
                };

                auto packet_cb = [&](std::shared_ptr<AVPacket>&& pkt) { packet_buffer.push(std::move(pkt)); };

                std::int32_t frame_number = 0;
                auto loop_start_time = std::chrono::high_resolution_clock::now();
                int64_t last_loop_time_us = 0;
                
                while (true) {
                    auto loop_iteration_start = std::chrono::high_resolution_clock::now();
                    
                    {
                        std::lock_guard<std::mutex> lock(state_mutex_);
                        state_["file/frame"] = frame_number++;
                    }

                    // TIMING: medir tempo de pop do buffer
                    auto pop_start = std::chrono::high_resolution_clock::now();
                    core::const_frame frame;
                    frame_buffer_.pop(frame);
                    auto pop_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - pop_start).count();
                    
                    graph_->set_value("input",
                                      static_cast<double>(frame_buffer_.size() + 0.001) / frame_buffer_.capacity());

                    caspar::timer frame_timer;
                    
                    // TIMING: medir tempo de encoding de vídeo
                    auto video_start = std::chrono::high_resolution_clock::now();
                    if (video_stream) {
                        video_stream->send(frame, format_desc, packet_cb);
                    }
                    auto video_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - video_start).count();
                    
                    // TIMING: medir tempo de encoding de áudio
                    auto audio_start = std::chrono::high_resolution_clock::now();
                    for (auto& audio_stream : audio_streams) {
                        if (audio_stream) {
                            audio_stream->send(frame, format_desc, packet_cb);
                        }
                    }
                    auto audio_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - audio_start).count();
                    
                    graph_->set_value("frame-time", frame_timer.elapsed() * format_desc.fps * 0.5);
                    
                    // TIMING: tempo total do loop
                    auto loop_time_us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::high_resolution_clock::now() - loop_iteration_start).count();
                    
                    // TIMING LOG: a cada 300 frames (~10 segundos) OU quando há problema
                    bool has_timing_problem = loop_time_us > 40000 || frame_buffer_.size() > 10;
                    if ((frame_number % 300 == 0) || has_timing_problem) {
                        int64_t avg_write_us = write_total_packets > 0 ? 
                            write_total_time_us / write_total_packets : 0;
                        
                        CASPAR_LOG(debug) << "[EXECUTOR] frame=" << frame_number
                            << " pop=" << pop_time_us
                            << " vid=" << video_time_us
                            << " aud=" << audio_time_us
                            << " loop=" << loop_time_us
                            << " wq=" << write_queue_size
                            << " wavg=" << avg_write_us
                            << (has_timing_problem ? " [SLOW]" : "");
                    }
                    last_loop_time_us = loop_time_us;

                    if (!frame) {
                        packet_buffer.push(nullptr);
                        break;
                    }
                }

                packet_thread.join();
            } catch (...) {
                std::lock_guard<std::mutex> lock(exception_mutex_);
                exception_ = std::current_exception();
            }
        });
    }

    // DEBUG: contadores para buffer
    mutable int64_t debug_frames_received_ = 0;
    mutable int64_t debug_frames_dropped_ = 0;
    mutable std::chrono::high_resolution_clock::time_point debug_consumer_start_time_;
    mutable bool debug_consumer_started_ = false;
    
    std::future<bool> send(core::const_frame frame) override
    {
        {
            std::lock_guard<std::mutex> lock(exception_mutex_);
            if (exception_ != nullptr) {
                std::rethrow_exception(exception_);
            }
        }
        
        // DEBUG: rastrear tempo e frames
        if (!debug_consumer_started_) {
            debug_consumer_start_time_ = std::chrono::high_resolution_clock::now();
            debug_consumer_started_ = true;
        }
        debug_frames_received_++;

        // BUFFER RECOVERY: Se buffer estiver muito cheio (>128), dropar frame preventivamente
        // para dar tempo ao encoder de recuperar, evitando colapso total
        const size_t BUFFER_RECOVERY_THRESHOLD = 128;  // 50% da capacidade
        const size_t BUFFER_CRITICAL_THRESHOLD = 200;  // ~78% - dropar mais agressivamente
        const size_t BUFFER_MAX_CAPACITY = 256;        // capacidade máxima do buffer
        
        bool should_drop_for_recovery = false;
        
        // IMPORTANTE: frame_buffer_.size() pode retornar valor inválido antes da inicialização
        // Usar conversão segura para evitar overflow quando size() retorna valor negativo
        auto raw_size = frame_buffer_.size();
        size_t current_buffer_size = (raw_size > 0 && raw_size <= BUFFER_MAX_CAPACITY) ? 
                                     static_cast<size_t>(raw_size) : 0;
        
        if (current_buffer_size >= BUFFER_CRITICAL_THRESHOLD) {
            // CRÍTICO: dropar 2 de cada 3 frames para recuperar rapidamente
            should_drop_for_recovery = (debug_frames_received_ % 3 != 0);
        } else if (current_buffer_size >= BUFFER_RECOVERY_THRESHOLD) {
            // ALERTA: dropar 1 de cada 3 frames para recuperar gradualmente
            should_drop_for_recovery = (debug_frames_received_ % 3 == 0);
        }
        
        if (should_drop_for_recovery) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "recovery-drop");
            debug_frames_dropped_++;
            
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - debug_consumer_start_time_).count();
            
            // Log de recuperação (menos alarmante que o erro de buffer cheio)
            if (debug_frames_dropped_ % 10 == 1) {  // Log a cada 10 drops
                CASPAR_LOG(warning) << "[BUFFER-RECOVERY] Frame " << debug_frames_received_ 
                    << " descartado preventivamente para recuperar encoder"
                    << " buffer=" << current_buffer_size << "/" << frame_buffer_.capacity()
                    << " total_dropped=" << debug_frames_dropped_
                    << " elapsed_ms=" << elapsed_ms;
            }
            // Não tenta push - frame descartado preventivamente
        }
        else if (!frame_buffer_.try_push(frame)) {
            graph_->set_tag(diagnostics::tag_severity::WARNING, "dropped-frame");
            debug_frames_dropped_++;
            
            // ERROR: buffer cheio - encoder não consegue acompanhar mesmo com recovery
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - debug_consumer_start_time_).count();
            double drop_rate = (double)debug_frames_dropped_ / debug_frames_received_ * 100.0;
            
            CASPAR_LOG(error) << "[FRAME-DROP] VIDEO COMPROMETIDO! Frame " << debug_frames_received_ 
                << " descartado - encoder muito lento!"
                << " total_dropped=" << debug_frames_dropped_
                << " drop_rate=" << std::fixed << std::setprecision(1) << drop_rate << "%"
                << " buffer_full=" << frame_buffer_.size() << "/" << frame_buffer_.capacity()
                << " elapsed_ms=" << elapsed_ms
                << " - Considere reduzir resolucao ou usar hardware mais rapido";
        }
        
        // DEBUG: log periódico do buffer status (a cada 30 frames)
        if (debug_frames_received_ % 30 == 0 || debug_frames_received_ <= 5) {
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::high_resolution_clock::now() - debug_consumer_start_time_).count();
            CASPAR_LOG(debug) << "[SYNC-DEBUG] BUFFER received=" << debug_frames_received_
                << " dropped=" << debug_frames_dropped_
                << " buffer_size=" << frame_buffer_.size()
                << " buffer_capacity=" << frame_buffer_.capacity()
                << " elapsed_ms=" << elapsed_ms;
        }
        
        graph_->set_value("input", static_cast<double>(frame_buffer_.size() + 0.001) / frame_buffer_.capacity());

        return make_ready_future(true);
    }

    std::wstring print() const override { return L"ffmpeg[" + u16(path_) + L"]"; }

    std::wstring name() const override { return L"ffmpeg"; }

    bool has_synchronization_clock() const override { return false; }

    int index() const override { return 100000 + channel_index_; }

    core::monitor::state state() const override
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return state_;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&                  params,
                                                      std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    if (params.size() < 2 || (!boost::iequals(params.at(0), L"STREAM") && !boost::iequals(params.at(0), L"FILE")))
        return core::frame_consumer::empty();

    auto                     path = u8(params.at(1));
    std::vector<std::string> args;
    for (auto n = 2; n < params.size(); ++n) {
        args.emplace_back(u8(params[n]));
    }
    return spl::make_shared<ffmpeg_consumer>(path, boost::join(args, " "), boost::iequals(params.at(0), L"STREAM"));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&               ptree,
                              std::vector<spl::shared_ptr<core::video_channel>> channels)
{
    return spl::make_shared<ffmpeg_consumer>(u8(ptree.get<std::wstring>(L"path", L"")),
                                             u8(ptree.get<std::wstring>(L"args", L"")),
                                             ptree.get(L"realtime", false));
}
}} // namespace caspar::ffmpeg
