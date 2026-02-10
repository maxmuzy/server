# CasparCG Server - Modificações XDCAM HD422

## Descrição das Modificações

Este fork do CasparCG Server foi modificado para gerar arquivos exclusivamente no formato **MOV XDCAM HD422** com as seguintes especificações:

### Especificações de Saída

| Parâmetro | Valor |
|-----------|-------|
| **Container** | MOV (QuickTime tradicional) |
| **Brand** | qt (Apple QuickTime) |
| **Perfil** | XDCAM HD422 |
| **Codec de Vídeo** | MPEG-2 4:2:2 |
| **Bitrate de Vídeo** | 50 Mbps CBR |
| **Pixel Format** | YUV422P |
| **Colorspace** | BT.709 |
| **Encoding** | Interlaced (top field first) |
| **GOP** | 12 frames (M=3, N=12) |
| **Matrix** | Padrão MPEG-2 (otimizado para performance) |
| **Codec de Áudio** | PCM 24-bit LPCM |
| **Sample Rate** | 48 kHz |
| **Canais de Áudio** | 4 canais mono separados (L, R, C, LFE) |
| **Timecode** | Trilha QuickTime TC |

### Arquivos Modificados

#### 1. `src/modules/ffmpeg/consumer/ffmpeg_consumer.cpp`

**Modificações:**

- **Container MOV QuickTime tradicional** (linhas ~573-590):
  ```cpp
  FF(avformat_alloc_output_context2(&oc, nullptr, "mov", full_path.string().c_str()));
  av_opt_set(oc->priv_data, "brand", "qt  ", 0);
  av_opt_set_int(oc->priv_data, "write_tmcd", 1, 0);
  ```

- **Extensão .mov forçada** (linhas ~549-552):
  ```cpp
  if (!boost::iequals(full_path.extension().string(), ".mov")) {
      full_path.replace_extension(".mov");
  }
  ```

- **Matrizes de quantização** (removido para otimização de performance):
  - Usa matrizes padrão do MPEG-2 em vez de customizadas
  - Resulta em encoding mais rápido mantendo qualidade 50Mbps

- **Acumulação de Callbacks para Interlaced** (linhas ~475-620):
  - CasparCG chama 60 callbacks/segundo para 1080i5994 (field rate)
  - XDCAM precisa de 30 frames/segundo (frame rate)
  - Solução: acumular 2 callbacks antes de processar
  - **Vídeo**: Processa apenas a cada 2º callback (pula callbacks ímpares)
  - **Áudio**: Acumula samples de 2 callbacks (~800+800=1600) antes de enviar
  - Usa `field_accumulator_count` para rastrear callbacks
  - Usa `audio_accumulator` (vector<int32_t>) para buffer de áudio
  - `is_interlaced_format = format_desc.field_count > 1` detecta interlaced

- **Frame rate corrigido para formatos interlaced** (linhas ~316-335):
  - Para 1080i5994: encoder configurado em 30000/1001 (29.97fps), não 60000/1001 (59.94fps)
  - Divisão do NUMERADOR: 60000/1001 → 30000/1001 (forma canônica)
  - Orçamento de tempo por frame: ~33ms (correto) em vez de ~16.6ms (errado)
  - Usa `format_desc.field_count > 1` para detectar interlaced
  - Suporta todos os framerates XDCAM: 59.94i, 50i, 29.97p, 25p, 23.98p (1080) e 59.94p, 50p (720p)

- **Codec MPEG-2 com parâmetros XDCAM** (linhas ~586-604):
  - Bitrate: 50 Mbps CBR (minrate/maxrate)
  - Buffer VBV: 17825792
  - GOP: 12 frames (N=12), B-frames: 2 (M=3) - reduzido de 15 para melhor performance
  - Flags interlaced: +ildct+ilme
  - Field order: Top Field First (enc->field_order = AV_FIELD_TT)
  - Codec tag: xd5b (enc->codec_tag = MKTAG('x','d','5','b'))
  - Intra VLC, DC precision 10-bit
  - QP range: 1-12
  - Scene change detection desabilitado (sc_threshold=1000000000)
  - B-frame strategy estático (b_strategy=0)
  - VBV control estrito (rc_max_vbv_use=1, rc_min_vbv_use=1)

- **Diagnóstico de Anomalias [DIAG]** (linhas ~140-146, ~594-630):
  - Contadores atômicos: `diag_frames_sent`, `diag_packets_received` para rastrear backlog do encoder
  - Rastreamento de intervalo de callbacks: `diag_callback_min_us`, `diag_callback_max_us`
  - **Backlog do encoder**: Diferença entre frames enviados e pacotes recebidos (threshold: >5)
  - **Anomalia de callback**: Intervalo < 10ms (rajada) ou > 50ms (atraso)
  - Log [DIAG] aparece apenas quando há anomalia OU a cada 300 frames (~10s)
  - Throttle de 150 frames (~5s) entre logs de anomalia para evitar spam
  - Tags: [SLOW], [MEM!], [BACKLOG!], [CB-ANOMALY]
  - Função `get_process_memory_mb()` para monitorar uso de memória
  - Alerta [MEM!] se delta > 200MB desde início

- **Buffer Recovery Mechanism** (linhas ~1122-1155):
  - Previne colapso do encoder quando buffer fica muito cheio
  - Threshold 128 frames (50%): dropa 1 de cada 3 frames
  - Threshold 200 frames (78%): dropa 2 de cada 3 frames
  - Permite que o encoder recupere antes de atingir 256/256
  - Log de warning "[BUFFER-RECOVERY]" quando ativado

- **Timing Granular do Executor** (linhas ~986-1130):
  - **Thread de escrita (packet_thread)**:
    - Contadores atômicos: `write_total_packets`, `write_total_time_us`, `write_max_time_us`, `write_queue_size`
    - Mede tempo de cada `av_interleaved_write_frame`
  - **Loop principal**:
    - `pop_us`: tempo de pop do buffer (espera por frames)
    - `video_us`: tempo de encoding de vídeo MPEG-2
    - `audio_us`: tempo de encoding de áudio PCM
    - `loop_us`: tempo total do loop de iteração
    - `write_queue`: tamanho da fila de pacotes pendentes
    - `write_avg_us/write_max_us`: estatísticas de escrita
  - Log [EXECUTOR] a cada 30 frames mostrando todas as métricas

- **Metadados de cor BT.709** (linhas ~297-300):
  ```cpp
  enc->color_primaries = AVCOL_PRI_BT709;
  enc->color_trc       = AVCOL_TRC_BT709;
  enc->colorspace      = AVCOL_SPC_BT709;
  ```

- **Top Field First no AVFrame** (linhas ~406-407):
  ```cpp
  frame2->interlaced_frame = 1;
  frame2->top_field_first  = 1;
  ```

- **Trilha de Timecode QuickTime TC** (linhas ~693-697):
  - write_tmcd=1 habilitado via av_opt_set e fallback via dicionário
  - Timecode baseado na hora do sistema (HH:MM:SS:FF)
  - Formato drop-frame (;) para 29.97fps, non-drop (:) para outros

- **4 Streams de Áudio Mono LPCM** (linhas ~710-718):
  - 4 tracks de áudio independentes (L, R, C, LFE)
  - Cada track: PCM S24LE, 48kHz, 1 canal (mono)
  - Reduzido de 8 para 4 canais para otimização de performance
  - Extração individual de cada canal do áudio fonte

- **Pixel format YUV422P** (várias linhas):
  - Buffer de vídeo: YUV422P
  - Frame de saída: YUV422P
  - Contexto SWS: YUV422P

#### 2. `src/modules/ffmpeg/util/av_util.cpp`

**Modificações:**

- **Função `make_av_audio_frame`** (linhas ~259-287):
  - Recebe parâmetro `channel_index` para extração de canal mono
  - Sample rate fixo em 48 kHz
  - Extrai canal específico do áudio fonte (ou silêncio se não existir)

#### 3. `src/CMakeLists.txt`

**Modificações:**

- Versão atualizada para 2.3.5

## Como Compilar

### Requisitos
- CMake 3.0+
- GCC/G++ ou Visual Studio 2017
- Docker (recomendado para Linux)

### Windows
```batch
mkdir build
cd build
cmake -G "Visual Studio 15 2017" -A x64 ../src
# Abrir CasparCG Server.sln no Visual Studio
```

### Linux (via Docker)
```bash
./tools/linux/build-in-docker
./tools/linux/extract-from-docker
```

## Uso

Após compilar, o consumer FFmpeg irá automaticamente gerar arquivos MOV XDCAM HD422.

### Exemplo de comando AMCP
```
ADD 1 FILE output.mov
```

### Verificação com MediaInfo

Deve mostrar:
- Format: QuickTime
- Format profile: QuickTime
- Commercial name: XDCAM HD422
- Format settings, Matrix: Custom
- Codec ID (video): xd5b
- Codec ID (audio): lpcm
- Frame rate: 29.970 (30000/1001) FPS
- Color primaries: BT.709
- Trilha de timecode: QuickTime TC

## Notas Importantes

1. **Todas as saídas do consumer FFmpeg serão MOV XDCAM HD422** - não há opção para outros formatos nesta versão modificada.

2. **Upmix de áudio automático**: Se a fonte tiver menos de 8 canais, os canais adicionais serão preenchidos com silêncio.

3. **Compatibilidade**: Os arquivos gerados são compatíveis com sistemas de edição profissional (Avid, Premiere, Final Cut, etc.) e automações de broadcast.

4. **Brand QuickTime**: O container é configurado com brand "qt  " para máxima compatibilidade com software Apple.

5. **Trilha de Timecode**: A trilha QuickTime TC é criada automaticamente com o timecode inicial baseado na hora do sistema.

---
*Modificado em: Dezembro 2025*
*Baseado em: CasparCG Server 2.3.5*
