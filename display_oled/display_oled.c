// Inclusão de bibliotecas necessárias
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "pico/stdlib.h"          // Biblioteca padrão do Pico
#include "hardware/adc.h"         // Para acesso ao ADC
#include "hardware/dma.h"         // Para acesso ao DMA
#include "hardware/timer.h"       // Para temporização
#include "inc/ssd1306.h"          // Para controle do display OLED
#include "hardware/i2c.h"         // Para comunicação I2C
#include "hardware/sync.h"        // Para sincronização

// Definições de constantes
#define NUM_SAMPLES 100           // Número de amostras por ciclo de leitura
#define UPDATE_INTERVAL_MS 500    // Intervalo de atualização em milissegundos

// Configuração dos pinos I2C para o OLED
const uint I2C_SDA = 14;          // Pino SDA
const uint I2C_SCL = 15;          // Pino SCL

// Buffer para armazenar as amostras do ADC
uint16_t adc_buffer[NUM_SAMPLES];

// Função para converter valor ADC para temperatura em Celsius
float convert_to_celsius(uint16_t raw) {
    const float conversion_factor = 3.3f / (1 << 12);  // Fator de conversão para tensão
    float voltage = raw * conversion_factor;           // Converte valor ADC para tensão
    return 27.0f - (voltage - 0.706f) / 0.001721f;    // Fórmula de conversão para temperatura
}

// Função para exibir temperatura no OLED
void display_temperature(float temp, uint8_t *ssd, struct render_area *area) {
    char temp_str[20];
    memset(ssd, 0, ssd1306_buffer_length);  // Limpa o buffer do display
    
    // Formata a string de temperatura
    snprintf(temp_str, sizeof(temp_str), "Temp: %.2f C", temp);
    
    // Desenha as strings no buffer do display
    ssd1306_draw_string(ssd, 5, 0, "SENSOR DE TEMP");
    ssd1306_draw_string(ssd, 5, 16, temp_str);
    ssd1306_draw_string(ssd, 5, 32, "RP2040 INTERNO");
    
    // Atualiza o display com o conteúdo do buffer
    render_on_display(ssd, area);
}

// Variável e callback para o timer de atualização
volatile bool timer_fired = false;  // Flag indicando que o timer disparou
bool alarm_callback(repeating_timer_t *rt) {
    timer_fired = true;             // Sinaliza que o timer disparou
    return true;                    // Continua repetindo o timer
}

// Função principal
int main() {
    stdio_init_all();               // Inicializa a comunicação serial via USB
    sleep_ms(2000);                // Aguarda 2 segundos para estabilização
    
    // Inicialização do I2C para o display OLED
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);         // Habilita pull-up nos pinos I2C
    gpio_pull_up(I2C_SCL);
    
    ssd1306_init();                // Inicializa o display OLED
    
    // Configura a área de renderização do display
    struct render_area frame_area = {
        start_column: 0,
        end_column: ssd1306_width - 1,
        start_page: 0,
        end_page: ssd1306_n_pages - 1
    };
    
    // Calcula o tamanho do buffer e inicializa
    calculate_render_area_buffer_length(&frame_area);
    uint8_t ssd[ssd1306_buffer_length];
    memset(ssd, 0, ssd1306_buffer_length);
    render_on_display(ssd, &frame_area);

    // Inicialização do ADC e sensor de temperatura interno
    adc_init();
    adc_set_temp_sensor_enabled(true);  // Habilita sensor de temperatura
    adc_select_input(4);                // Seleciona canal 4 (sensor de temperatura)

    // Configuração do DMA para transferência eficiente das amostras
    int dma_chan = dma_claim_unused_channel(true);
    dma_channel_config cfg = dma_channel_get_default_config(dma_chan);
    
    // Configuração do canal DMA
    channel_config_set_transfer_data_size(&cfg, DMA_SIZE_16);
    channel_config_set_read_increment(&cfg, false);
    channel_config_set_write_increment(&cfg, true);
    channel_config_set_dreq(&cfg, DREQ_ADC);  // Usa ADC como requisição de dados

    // Configura timer para intervalos regulares de atualização
    repeating_timer_t timer;
    add_repeating_timer_ms(UPDATE_INTERVAL_MS, alarm_callback, NULL, &timer);

    // Loop principal
    while (true) {
        // Aguarda o próximo intervalo de atualização
        while (!timer_fired) {
            __wfi();  // Entra em modo de espera para economizar energia
        }
        timer_fired = false;

        // Prepara o ADC para nova leitura
        adc_fifo_drain();  // Esvazia o FIFO do ADC
        adc_run(false);     // Pausa o ADC
        
        // Configura o FIFO do ADC
        adc_fifo_setup(true, true, 1, false, false);
        adc_run(true);      // Inicia o ADC
        
        // Configura e inicia a transferência DMA
        dma_channel_configure(
            dma_chan,
            &cfg,
            adc_buffer,     // Buffer de destino
            &adc_hw->fifo, // Fonte de dados (FIFO do ADC)
            NUM_SAMPLES,   // Número de amostras
            true          // Inicia imediatamente
        );
        
        // Aguarda conclusão da transferência DMA
        dma_channel_wait_for_finish_blocking(dma_chan);
        adc_run(false);     // Pausa o ADC após a leitura
        
        // Calcula a temperatura média
        float sum = 0.0f;
        for (int i = 0; i < NUM_SAMPLES; i++) {
            sum += convert_to_celsius(adc_buffer[i]);
        }
        float avg_temp = sum / NUM_SAMPLES;
        
        // Exibe a temperatura no terminal e no display
        printf("Temperatura média: %.2f °C\n", avg_temp);
        display_temperature(avg_temp, ssd, &frame_area);
    }

    // Limpeza (não deve ser alcançado em operação normal)
    cancel_repeating_timer(&timer);
    return 0;
}