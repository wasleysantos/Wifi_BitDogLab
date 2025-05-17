#include <string.h>
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/pbuf.h"
#include "lwip/tcp.h"
#include "dhcpserver.h"
#include "dnsserver.h"
#include "hardware/i2c.h"
#include "ssd1306.h"

#define TCP_PORT 80
#define POLL_TIME_S 5
#define HTTP_GET "GET"
#define HTTP_RESPONSE_HEADERS "HTTP/1.1 200 OK\nContent-Length: %d\nContent-Type: text/html\nConnection: close\n\n"
#define HTTP_RESPONSE_REDIRECT "HTTP/1.1 302 Found\nLocation: http://%s/bitdoglabtest\n\n"

// GPIOs dos LEDs RGB BitDogLab
#define LED_RED 13
#define LED_GREEN 11
#define LED_BLUE 12
#define BUZZER 10 // NOVO: GPIO do buzzer

const uint I2C_SDA = 14;
const uint I2C_SCL = 15;
uint8_t ssd[ssd1306_buffer_length];
struct render_area frame_area;

typedef struct TCP_SERVER_T_ {
    struct tcp_pcb *server_pcb;
    bool complete;
    ip_addr_t gw;
} TCP_SERVER_T;

typedef struct TCP_CONNECT_STATE_T_ {
    struct tcp_pcb *pcb;
    int sent_len;
    char headers[128];
    char result[1024];
    int header_len;
    int result_len;
    ip_addr_t *gw;
} TCP_CONNECT_STATE_T;

// Função para desenhar texto com escala
void ssd1306_draw_string_scaled(uint8_t *buffer, int x, int y, const char *text, int scale) {
    while (*text) {
        for (int dx = 0; dx < scale; dx++) {
            for (int dy = 0; dy < scale; dy++) {
                ssd1306_draw_char(buffer, x + dx, y + dy, *text);
            }
        }
        x += 6 * scale;
        text++;
    }
}

// Inicializa os LEDs e o buzzer
void init_leds() {
    gpio_init(LED_RED);
    gpio_set_dir(LED_RED, GPIO_OUT);
    gpio_put(LED_RED, 0);

    gpio_init(LED_GREEN);
    gpio_set_dir(LED_GREEN, GPIO_OUT);
    gpio_put(LED_GREEN, 0);

    gpio_init(LED_BLUE);
    gpio_set_dir(LED_BLUE, GPIO_OUT);
    gpio_put(LED_BLUE, 0);

    gpio_init(BUZZER); // NOVO
    gpio_set_dir(BUZZER, GPIO_OUT);
    gpio_put(BUZZER, 0);
}

// Processa parâmetros da URL
void parse_params(const char *params) {
    int r, g, b;
    if (sscanf(params, "red=%d&green=%d&blue=%d", &r, &g, &b) == 3) {
        gpio_put(LED_RED, r);
        gpio_put(LED_GREEN, g);
        gpio_put(LED_BLUE, b);
    } else {
        if (strstr(params, "red=1")) gpio_put(LED_RED, 1);
        if (strstr(params, "red=0")) gpio_put(LED_RED, 0);
        if (strstr(params, "green=1")) gpio_put(LED_GREEN, 1);
        if (strstr(params, "green=0")) gpio_put(LED_GREEN, 0);
        if (strstr(params, "blue=1")) gpio_put(LED_BLUE, 1);
        if (strstr(params, "blue=0")) gpio_put(LED_BLUE, 0);
    }

    // NOVO: controle do buzzer
    if (strstr(params, "buzzer=1")) gpio_put(BUZZER, 1);
    if (strstr(params, "buzzer=0")) gpio_put(BUZZER, 0);
}

// Gera o conteúdo HTML de resposta
int generate_html(char *result, size_t max_len) {
    bool r = gpio_get(LED_RED);
    bool g = gpio_get(LED_GREEN);
    bool b = gpio_get(LED_BLUE);
    bool bz = gpio_get(BUZZER); // NOVO

    return snprintf(result, max_len,
    "<html>"
    "<head>"
    "<style>"
    "body {"
    "background-color: #ADD8E6;" /* azul claro */
    "font-family: Arial, sans-serif;"
    "text-align: center;"
    "margin: 0;"
    "padding: 20px;"
    "}"
    "h2 {"
    "color: #333;"
    "}"
    ".status {"
    "margin: 15px 0;"
    "font-size: 1.2em;"
    "}"
    ".btn {"
    "background-color: #007BFF;"
    "color: white;"
    "border: none;"
    "padding: 10px 20px;"
    "margin-top: 5px;"
    "text-decoration: none;"
    "border-radius: 5px;"
    "cursor: pointer;"
    "font-size: 1em;"
    "transition: background-color 0.3s;"
    "}"
    ".btn:hover {"
    "background-color: #0056b3;"
    "}"
    "</style>"
    "</head>"
    "<body>"
    "<h2>Painel de Controle BitDogLab</h2>"

    "<div class='status'>"
    "<p>LED Vermelho: %s</p>"
    "<a class='btn' href=\"?red=%d\">%s</a>"
    "</div>"
    "<div class='status'>"
    "<p>LED Verde: %s</p>"
    "<a class='btn' href=\"?green=%d\">%s</a>"
    "</div>"
    "<div class='status'>"
    "<p>LED Azul: %s</p>"
    "<a class='btn' href=\"?blue=%d\">%s</a>"
    "</div>"
    "<div class='status'>"
    "<p>Buzzer: %s</p>"
    "<a class='btn' href=\"?buzzer=%d\">%s</a>"
    "</div>"

    "</body>"
    "</html>",

    r ? "Ligado" : "Desligado", r ? 0 : 1, r ? "Desligar" : "Ligar",
    g ? "Ligado" : "Desligado", g ? 0 : 1, g ? "Desligar" : "Ligar",
    b ? "Ligado" : "Desligado", b ? 0 : 1, b ? "Desligar" : "Ligar",
    bz ? "Ligado" : "Desligado", bz ? 0 : 1, bz ? "Desligar" : "Ligar"
);
}

// Trata requisição HTTP
int handle_request(const char *request, const char *params, char *result, size_t max_len) {
    if (strncmp(request, "/bitdoglabtest", 8) == 0) {
        if (params) parse_params(params);
        return generate_html(result, max_len);
    }
    return 0;
}

err_t tcp_server_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    TCP_CONNECT_STATE_T *con_state = (TCP_CONNECT_STATE_T*)arg;
    if (!p) return tcp_close(pcb);

    pbuf_copy_partial(p, con_state->headers, sizeof(con_state->headers) - 1, 0);
    char *request_line = strtok(con_state->headers, "\r\n");
    char *method = strtok(request_line, " ");
    char *url = strtok(NULL, " ");
    char *params = strchr(url, '?');
    if (params) {
        *params = 0;
        params++;
    }

    con_state->result_len = handle_request(url, params, con_state->result, sizeof(con_state->result));

    if (con_state->result_len > 0) {
        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_HEADERS, con_state->result_len);
    } else {
        con_state->header_len = snprintf(con_state->headers, sizeof(con_state->headers), HTTP_RESPONSE_REDIRECT, ipaddr_ntoa(con_state->gw));
        con_state->result_len = 0;
    }

    con_state->sent_len = 0;
    tcp_write(pcb, con_state->headers, con_state->header_len, 0);
    if (con_state->result_len > 0) tcp_write(pcb, con_state->result, con_state->result_len, 0);

    tcp_recved(pcb, p->tot_len);
    pbuf_free(p);
    return tcp_close(pcb);
}

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)arg;
    TCP_CONNECT_STATE_T *con_state = calloc(1, sizeof(TCP_CONNECT_STATE_T));
    con_state->pcb = client_pcb;
    con_state->gw = &state->gw;
    tcp_arg(client_pcb, con_state);
    tcp_recv(client_pcb, tcp_server_recv);
    return ERR_OK;
}

static bool tcp_server_open(TCP_SERVER_T *state, const char *ap_name) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb) return false;
    if (tcp_bind(pcb, IP_ANY_TYPE, TCP_PORT)) return false;

    state->server_pcb = tcp_listen_with_backlog(pcb, 1);
    tcp_arg(state->server_pcb, state);
    tcp_accept(state->server_pcb, tcp_server_accept);

    printf("Acesse: http://%s/bitdoglabtest\n", ap_name);
    return true;
}

void key_pressed_func(void *param) {
    TCP_SERVER_T *state = (TCP_SERVER_T*)param;
    int key = getchar_timeout_us(0);
    if (key == 'd' || key == 'D') {
        cyw43_arch_lwip_begin();
        cyw43_arch_disable_ap_mode();
        cyw43_arch_lwip_end();
        state->complete = true;
    }
}

int main() {
    stdio_init_all();
    TCP_SERVER_T *state = calloc(1, sizeof(TCP_SERVER_T));
    cyw43_arch_init();
    stdio_set_chars_available_callback(key_pressed_func, state);

    // Inicializa I2C e display
    i2c_init(i2c1, ssd1306_i2c_clock * 1000);
    gpio_set_function(I2C_SDA, GPIO_FUNC_I2C);
    gpio_set_function(I2C_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(I2C_SDA);
    gpio_pull_up(I2C_SCL);
    ssd1306_init();

    // Define área de renderização
    frame_area.start_column = 0;
    frame_area.end_column = ssd1306_width - 1;
    frame_area.start_page = 0;
    frame_area.end_page = ssd1306_n_pages - 1;
    calculate_render_area_buffer_length(&frame_area);

    // Limpa buffer
    memset(ssd, 0, ssd1306_buffer_length);

    // Define escala
    int scale = 2;

    // Calcula posição horizontal centralizada para ambas as palavras
    const char *linha1 = "Controle";
    const char *linha2 = "Wifi";

    int largura_hello = strlen(linha1) * 6 * scale;
    int largura_world = strlen(linha2) * 6 * scale;

    int pos_x_hello = (ssd1306_width - largura_hello) / 2;
    int pos_x_world = (ssd1306_width - largura_world) / 2;

    // Define posições Y
    int pos_y_hello = 10;
    int pos_y_world = 35;

    // Desenha ambas as palavras
    ssd1306_draw_string_scaled(ssd, pos_x_hello, pos_y_hello, linha1, scale);
    ssd1306_draw_string_scaled(ssd, pos_x_world, pos_y_world, linha2, scale);

    // Renderiza no display
    render_on_display(ssd, &frame_area);

    init_leds();

    const char *ap_name = "BitDogLab Wasley";
    const char *password = "12345678";

    cyw43_arch_enable_ap_mode(ap_name, password, CYW43_AUTH_WPA2_AES_PSK);

    ip4_addr_t mask;
    IP4_ADDR(&state->gw, 192, 168, 4, 1);
    IP4_ADDR(&mask, 255, 255, 255, 0);

    dhcp_server_t dhcp_server;
    dhcp_server_init(&dhcp_server, &state->gw, &mask);

    dns_server_t dns_server;
    dns_server_init(&dns_server, &state->gw);

    if (!tcp_server_open(state, "192.168.4.1")) return 1;

    state->complete = false;
    while (!state->complete) {
        sleep_ms(1000);
    }

    cyw43_arch_deinit();
    return 0;
}
