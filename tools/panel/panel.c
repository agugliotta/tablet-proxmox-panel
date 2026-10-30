// SPDX-License-Identifier: GPL-2.0-or-later
#include "lvgl.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#ifdef __linux__
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <poll.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#endif

enum { SUMMARY, MACHINES, SYSTEM, SHUTDOWN, RESTART, SENT };
enum { FILTER_ALL=100, FILTER_RUNNING, FILTER_STOPPED, FILTER_UNKNOWN };
static PanelData model = {
    .host="pve-homelab", .cpu_percent=23, .host_state=HOST_OK, .demo=1, .ram_used=12.4, .ram_total=32,
    .disk_used=186, .disk_total=512,
    .machines={{"Home Assistant", "VM", 100, RUNNING},
               {"DNS local", "CT", 101, RUNNING},
               {"Servidor de archivos", "VM", 102, RUNNING},
               {"Laboratorio", "VM", 103, STOPPED},
               {"Copias de seguridad", "CT", 104, UNKNOWN}}, .count=5
};
static const uint32_t bg=0x101820, card_bg=0x1b2834, white=0xf1f5f9;
static const uint32_t muted=0xa9bacb, orange=0xffa34a, green=0x72ddb6;
static lv_obj_t *root;
static lv_indev_t *touch;
static int current, preview, sent, mock_requests, expected_power;
static volatile sig_atomic_t stopping;
static PanelStream stream;
static int usb_mode, refresh_pending;
static lv_obj_t *machine_list;
static int32_t machine_scroll;
static int machine_filter=FILTER_ALL;
static size_t visible_machines;
static lv_obj_t *connection_badge;
static lv_obj_t *transport_label;
#ifdef __linux__
static pid_t power_child;
#endif

static void show(int page);
static void update_transport(void);
static void stop(int sig) { (void)sig; stopping=1; }
static uint32_t ticks(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint32_t)((uint64_t)t.tv_sec*1000+t.tv_nsec/1000000);
}

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h, uint32_t color)
{
    lv_obj_t *o=lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o,x,y); lv_obj_set_size(o,w,h);
    lv_obj_set_style_bg_color(o,lv_color_hex(color),0);
    lv_obj_set_style_bg_opa(o,LV_OPA_COVER,0);
    lv_obj_set_style_radius(o,16,0);
    lv_obj_remove_flag(o,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_update_layout(o);
    return o;
}

static lv_obj_t *label(lv_obj_t *parent, int x, int y, int w,
                       const char *text, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *o=lv_label_create(parent);
    lv_obj_set_pos(o,x,y); lv_obj_set_width(o,w);
    lv_obj_set_style_text_font(o,font,0);
    lv_obj_set_style_text_color(o,lv_color_hex(color),0);
    lv_label_set_text(o,text);
    lv_obj_update_layout(o);
    return o;
}

static void home(void)
{
    if(sent) return;
    if(touch) lv_indev_reset(touch,NULL);
    show(SUMMARY);
    machine_scroll=0;
}

static int request_power(int action)
{
    if(action!=SHUTDOWN && action!=RESTART) return -EINVAL;
    if(preview) { assert(action==expected_power); mock_requests++; return 0; }
#ifdef __linux__
    struct utsname u;
    if(uname(&u)<0) return -errno;
    if(!strstr(u.release,"-eutb710mdq-sd4wifi") &&
       !strstr(u.release,"-eutb710mdq-sd3power")) return -ENOTSUP;
    power_child=fork();
    if(power_child<0) return -errno;
    if(!power_child) {
        execl(action==SHUTDOWN?"/sbin/poweroff":"/sbin/reboot",
              action==SHUTDOWN?"poweroff":"reboot",(char *)NULL);
        _exit(127);
    }
    return 0;
#else
    return -ENOTSUP;
#endif
}

static void clicked(lv_event_t *e)
{
    int action=(int)(intptr_t)lv_event_get_user_data(e);
    lv_indev_t *input=lv_event_get_indev(e);
    lv_point_t point;
    if(!input) return;
    lv_indev_get_point(input,&point);
    if(!lv_obj_hit_test(lv_event_get_target_obj(e),&point)) return;
    if(sent) return;
    if(action>=FILTER_ALL && action<=FILTER_UNKNOWN) {
        machine_filter=action;
        if(machine_list) lv_obj_scroll_to_y(machine_list,0,LV_ANIM_OFF);
        machine_scroll=0;
        show(MACHINES);
        return;
    }
    if(action<0) {
        // A separate click on the confirmation is required; no press/repeat action.
        int rc=request_power(-action);
        if(!rc) { sent=1; show(SENT); }
        else label(root,32,474,950,"No se pudo solicitar la accion. HOME para volver.",
                   &lv_font_montserrat_20,orange);
    } else show(action);
}

static lv_obj_t *button(int x, int y, int w, int h, const char *text,
                        int action, uint32_t color)
{
    lv_obj_t *o=box(root,x,y,w,h,color);
    lv_obj_add_flag(o,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_style_bg_color(o,lv_color_hex(0x506476),LV_STATE_PRESSED);
    lv_obj_t *t=label(o,0,0,w,text,&lv_font_montserrat_24,white);
    lv_obj_set_style_text_align(t,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_center(t);
    lv_obj_add_event_cb(o,clicked,LV_EVENT_CLICKED,(void *)(intptr_t)action);
    return o;
}

static void metric(int x, const char *title, const char *value,
                   const char *detail, int percent)
{
    lv_obj_t *o=box(root,x,206,304,232,card_bg);
    label(o,24,22,260,title,&lv_font_montserrat_24,muted);
    label(o,24,68,266,value,&lv_font_montserrat_48,white);
    label(o,24,133,266,detail,&lv_font_montserrat_20,muted);
    if(percent>=0) {
        lv_obj_t *bar=lv_bar_create(o);
        lv_obj_set_pos(bar,24,190); lv_obj_set_size(bar,256,10);
        lv_obj_set_style_bg_color(bar,lv_color_hex(0x334657),0);
        lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,0);
        lv_obj_set_style_bg_color(bar,lv_color_hex(orange),LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar,LV_OPA_COVER,LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar,5,0);
        lv_obj_set_style_radius(bar,5,LV_PART_INDICATOR);
        lv_bar_set_value(bar,percent,LV_ANIM_OFF);
    }
}

static void show(int page)
{
    if(machine_list) machine_scroll=lv_obj_get_scroll_y(machine_list);
    machine_list=NULL;
    current=page;
    refresh_pending=0;
    transport_label=NULL;
    connection_badge=NULL;
    lv_obj_clean(root);
    label(root,32,23,560,"PROXMOX / HOMELAB",&lv_font_montserrat_24,white);
    lv_obj_t *badge=box(root,740,18,252,42,0x443326);
    connection_badge=label(badge,16,10,230,!usb_mode?"DEMOSTRACION":!stream.valid?"ESPERANDO USB":
          model.demo?"DEMO / USB":"EN VIVO / USB",&lv_font_montserrat_20,orange);
    const char *titles[]={"Resumen del host", "Maquinas", "Esta tablet",
                          "Apagar esta tablet?", "Reiniciar esta tablet?", "Accion solicitada"};
    if(page!=MACHINES) {
        label(root,32,94,960,titles[page],&lv_font_montserrat_32,white);
        lv_obj_t *subtitle=label(root,32,146,960,
          page==SUMMARY?"Datos simulados / sin conexion a Proxmox":
          page==MACHINES?"Estados de ejemplo / consulta de solo lectura":
          page==SYSTEM?"Controles locales / no afectan al host Proxmox":
          page==SENT?"Espera mientras el sistema completa la solicitud.":
          "Se perderan los cambios temporales. HOME cancela.",
          &lv_font_montserrat_20,muted);
        if(usb_mode && page==SUMMARY) transport_label=subtitle;
    }
    update_transport();
    if(page==SUMMARY) {
        char cpu[32],ram[32],disk[32],ram_detail[48],disk_detail[48];
        snprintf(cpu,sizeof(cpu),model.cpu_percent<0?"Sin datos":"%d %%",model.cpu_percent);
        snprintf(ram,sizeof(ram),model.ram_total<=0?"Sin datos":model.ram_used>=1024?"%.1f TiB":"%.1f GiB",model.ram_used>=1024?model.ram_used/1024:model.ram_used);
        snprintf(disk,sizeof(disk),model.disk_total<=0?"Sin datos":model.disk_used>=1024?"%.1f TiB":"%.0f GiB",model.disk_used>=1024?model.disk_used/1024:model.disk_used);
        snprintf(ram_detail,sizeof(ram_detail),model.ram_total<=0?"Sin datos":"de %.0f GiB totales",model.ram_total);
        snprintf(disk_detail,sizeof(disk_detail),model.disk_total<=0?"Sin datos":"%.32s / %.0f GiB",model.storage[0]?model.storage:"Total",model.disk_total);
        metric(32,"CPU",cpu,"Uso del procesador",model.cpu_percent);
        metric(360,"MEMORIA",ram,ram_detail,model.ram_total>0?(int)(100*model.ram_used/model.ram_total):-1);
        metric(688,"ALMACENAMIENTO",disk,disk_detail,model.disk_total>0?(int)(100*model.disk_used/model.disk_total):-1);
        char status[160];
        snprintf(status,sizeof(status),"%s  /  Estado general: %s",model.host,
                 model.host_state==HOST_OK?(model.demo?"ejemplo operativo":"operativo"):
                 model.host_state==HOST_ERROR?"error de consulta":"Sin datos");
        label(root,32,468,960,status,&lv_font_montserrat_20,model.host_state==HOST_OK?green:orange);
    } else if(page==MACHINES) {
        const char *filters[]={"Todas","En ejecucion","Detenidas","Sin datos"};
        for(int i=0;i<4;i++) button(32+i*244,94,228,76,filters[i],FILTER_ALL+i,
                                   machine_filter==FILTER_ALL+i?0x8c4c20:card_bg);
        machine_list=box(root,32,196,960,312,bg);
        lv_obj_add_flag(machine_list,LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(machine_list,LV_DIR_VER);
        lv_obj_set_scrollbar_mode(machine_list,LV_SCROLLBAR_MODE_AUTO);
        lv_obj_set_style_bg_color(machine_list,lv_color_hex(muted),LV_PART_SCROLLBAR);
        lv_obj_set_style_bg_opa(machine_list,LV_OPA_COVER,LV_PART_SCROLLBAR);
        lv_obj_set_style_width(machine_list,7,LV_PART_SCROLLBAR);
        lv_obj_set_style_radius(machine_list,4,LV_PART_SCROLLBAR);
        visible_machines=0;
        for(size_t i=0;i<model.count;i++) {
            Machine *m=&model.machines[i];
            if(machine_filter!=FILTER_ALL && m->state!=(machine_filter==FILTER_RUNNING?RUNNING:
               machine_filter==FILTER_STOPPED?STOPPED:UNKNOWN)) continue;
            lv_obj_t *row=box(machine_list,0,(int)visible_machines++*61,944,53,card_bg);
            char id[32]; snprintf(id,sizeof(id),"%s %d",m->kind,m->id);
            label(row,16,14,170,id,&lv_font_montserrat_20,muted);
            lv_obj_t *name=label(row,200,12,500,m->name,&lv_font_montserrat_24,white);
            lv_label_set_long_mode(name,LV_LABEL_LONG_DOT); lv_obj_set_height(name,30);
            label(row,712,14,240,m->state==RUNNING?"En ejecucion":
                  m->state==STOPPED?"Detenida":"Sin datos",&lv_font_montserrat_20,
                  m->state==RUNNING?green:muted);
        }
        if(!visible_machines) label(machine_list,0,34,940,model.count?
            "No hay maquinas en este estado.":"Sin datos de maquinas.",&lv_font_montserrat_24,muted);
        lv_obj_update_layout(machine_list);
        int32_t max_scroll=(int32_t)visible_machines*61-8-312;
        if(max_scroll<0) max_scroll=0;
        if(machine_scroll<0) machine_scroll=0;
        if(machine_scroll>max_scroll) machine_scroll=max_scroll;
        lv_obj_scroll_to_y(machine_list,machine_scroll,LV_ANIM_OFF);
    } else if(page==SYSTEM) {
        lv_obj_t *o=box(root,32,206,960,164,card_bg);
        label(o,24,20,910,"Linux / Allwinner A23 / 1024 x 600",&lv_font_montserrat_24,white);
        label(o,24,66,910,"Sistema en SD de solo lectura. Datos recibidos por USB.",
              &lv_font_montserrat_20,muted);
        label(o,24,108,910,"HOME vuelve a Resumen y cancela las confirmaciones.",
              &lv_font_montserrat_20,muted);
        button(32,398,468,88,"Apagar tablet",SHUTDOWN,0x75432f);
        button(524,398,468,88,"Reiniciar tablet",RESTART,0x30495e);
    } else if(page==SHUTDOWN || page==RESTART) {
        label(root,32,228,950,"Confirma con un segundo toque o vuelve a Resumen.",
              &lv_font_montserrat_24,white);
        button(32,350,468,96,"Cancelar",SUMMARY,0x30495e);
        button(524,350,468,96,"Confirmar",-page,0x75432f);
    }
    if(page<=SYSTEM) {
        const char *nav[]={"Resumen","Maquinas","Sistema"};
        for(int i=0;i<3;i++) button(32+i*328,526,304,58,nav[i],i,
                                    page==i?0x8c4c20:card_bg);
    }
    lv_obj_update_layout(root);
    lv_refr_now(NULL);
    lv_obj_invalidate(root);
}

static void update_transport(void)
{
    if(usb_mode && connection_badge) {
        const char *badge=!stream.valid?"ESPERANDO DATOS":model.host_state==HOST_ERROR?"ERROR PROXMOX":
            panel_stream_stale(&stream,ticks())?"DATOS ANTIGUOS":model.demo?"DEMOSTRACION":
            current==MACHINES?"EN VIVO":"EN VIVO / USB";
        if(strcmp(lv_label_get_text(connection_badge),badge)) lv_label_set_text(connection_badge,badge);
    }
    if(!transport_label) return;
    char message[160];
    int stale=panel_stream_stale(&stream,ticks());
    if(!stream.valid) snprintf(message,sizeof(message),"USB: esperando datos del host / Sin datos");
    else if(model.host_state==HOST_ERROR && !stream.samples)
        snprintf(message,sizeof(message),"USB: error de consulta a Proxmox / Sin datos");
    else snprintf(message,sizeof(message),"USB: %s / ultima muestra hace %u s / %s",
                  model.host_state==HOST_ERROR?"ERROR PROXMOX":stale?"DESACTUALIZADO":"recibiendo",
                  (unsigned)(ticks()-stream.last_valid)/1000,model.demo?"datos simulados":"datos reales");
    if(strcmp(lv_label_get_text(transport_label),message)) lv_label_set_text(transport_label,message);
    lv_obj_set_style_text_color(transport_label,lv_color_hex(stale || model.host_state==HOST_ERROR?orange:muted),0);
}

static void refresh_data(int changed)
{
    refresh_pending|=changed;
    // Preserve an in-progress gesture and every power confirmation across USB updates.
    if(refresh_pending && current<=MACHINES && lv_indev_get_state(touch)!=LV_INDEV_STATE_PRESSED &&
       (!machine_list || !lv_obj_is_scrolling(machine_list))) show(current);
    update_transport();
}

// The exact same LVGL widgets render into this memory display for offline QA.
static uint32_t pixels[1024*600], drawbuf[1024*60];
static void flush(lv_display_t *display, const lv_area_t *a, uint8_t *data)
{
    for(int y=a->y1;y<=a->y2;y++) {
        memcpy(&pixels[y*1024+a->x1],data,(size_t)(a->x2-a->x1+1)*4);
        data+=(a->x2-a->x1+1)*4;
    }
    lv_display_flush_ready(display);
}

static void capture(const char *dir, const char *name)
{
    lv_refr_now(NULL);
    char path[1024];
    assert(snprintf(path,sizeof(path),"%s/%s.ppm",dir,name)<(int)sizeof(path));
    FILE *f=fopen(path,"wb"); assert(f);
    fprintf(f,"P6\n1024 600\n255\n");
    for(size_t i=0;i<1024*600;i++) {
        uint8_t rgb[]={(uint8_t)(pixels[i]>>16),(uint8_t)(pixels[i]>>8),(uint8_t)pixels[i]};
        assert(fwrite(rgb,1,3,f)==3);
    }
    assert(!fclose(f));
}

static lv_indev_data_t simulated;
static void read_simulated(lv_indev_t *in, lv_indev_data_t *data)
{ (void)in; *data=simulated; }
static void pointer(int x, int y, int pressed)
{
    simulated.point=(lv_point_t){x,y};
    simulated.state=pressed?LV_INDEV_STATE_PRESSED:LV_INDEV_STATE_RELEASED;
    lv_indev_read(touch);
}
static void tap(int x, int y) { pointer(x,y,1); pointer(x,y,0); }

static int preview_all(const char *dir)
{
    preview=1;
    lv_display_t *d=lv_display_create(1024,600); assert(d);
    lv_display_set_color_format(d,LV_COLOR_FORMAT_XRGB8888);
    lv_display_set_buffers(d,drawbuf,NULL,sizeof(drawbuf),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    root=lv_screen_active();
    lv_obj_set_style_bg_color(root,lv_color_hex(bg),0);
    lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    touch=lv_indev_create(); lv_indev_set_type(touch,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch,read_simulated);
    show(SUMMARY); capture(dir,"resumen");
    tap(450,554); assert(current==MACHINES); capture(dir,"maquinas");
    tap(800,554); assert(current==SYSTEM); capture(dir,"sistema");
    for(int action=SHUTDOWN;action<=RESTART;action++) {
        show(SYSTEM);
        tap(action==SHUTDOWN?200:700,440); assert(current==action);
        capture(dir,action==SHUTDOWN?"apagar":"reiniciar");
        pointer(700,400,1); home(); pointer(700,400,0);
        assert(current==SUMMARY && !mock_requests); // Home invalidates held confirmation.
        show(SYSTEM);
        pointer(200,440,1); pointer(700,440,1); pointer(700,440,0);
        assert(current==SYSTEM); // A slide between buttons must not trigger power.
    }
    for(int action=SHUTDOWN;action<=RESTART;action++) {
        sent=0; expected_power=action;
        show(SYSTEM); tap(action==SHUTDOWN?200:700,440);
        assert(current==action); tap(700,400); assert(current==SENT && sent);
        home(); tap(700,400); assert(current==SENT);
    }
    assert(mock_requests==2);
    sent=0; model.cpu_percent=-1; model.ram_total=0; model.disk_total=0;
    model.host_state=HOST_UNKNOWN;
    show(SUMMARY); capture(dir,"sin-datos");
    usb_mode=1; show(SUMMARY); capture(dir,"usb-esperando");
    stream.valid=1; stream.samples=1; stream.last_valid=ticks(); model.cpu_percent=67;
    refresh_data(1); capture(dir,"usb-recibiendo");
    stream.last_valid=ticks()-11000; update_transport(); capture(dir,"usb-desactualizado");
    assert(strstr(lv_label_get_text(transport_label),"DESACTUALIZADO"));
    stream.last_valid=ticks(); update_transport();
    assert(strstr(lv_label_get_text(transport_label),"recibiendo"));
    show(SHUTDOWN); refresh_data(1); assert(current==SHUTDOWN); home();
    pointer(450,554,1); refresh_data(1); assert(current==SUMMARY && refresh_pending);
    pointer(450,554,0); assert(current==MACHINES); home();
    model.demo=0; model.host_state=HOST_OK; strcpy(model.storage,"local-lvm");
    model.ram_total=64; model.ram_used=12; model.disk_total=816; model.disk_used=627;
    stream.last_valid=ticks(); show(SUMMARY); capture(dir,"real-resumen");
    for(size_t i=0;i<19;i++) {
        model.machines[i]=(Machine){.kind="CT",.id=100+(int)i,.state=i<17?RUNNING:STOPPED};
        snprintf(model.machines[i].name,sizeof(model.machines[i].name),"Servicio %zu",i+1);
    }
    model.count=19; show(MACHINES); capture(dir,"real-maquinas");
    assert(!transport_label && !strstr(lv_label_get_text(connection_badge),"USB"));
    tap(390,130); assert(machine_filter==FILTER_RUNNING && visible_machines==17);
    lv_obj_scroll_to_y(machine_list,500,LV_ANIM_OFF);
    refresh_data(1); assert(machine_filter==FILTER_RUNNING && visible_machines==17);
    assert(lv_obj_get_scroll_y(machine_list)==500);
    capture(dir,"filtro-ejecucion");
    tap(630,130); assert(machine_filter==FILTER_STOPPED && visible_machines==2);
    assert(lv_obj_get_scroll_y(machine_list)==0); capture(dir,"filtro-detenidas");
    model.machines[17].state=RUNNING; refresh_data(1);
    assert(machine_filter==FILTER_STOPPED && visible_machines==1);
    model.machines[17].state=STOPPED;
    tap(875,130); assert(machine_filter==FILTER_UNKNOWN && !visible_machines);
    capture(dir,"filtro-vacio");
    model.machines[0].state=UNKNOWN; refresh_data(1); assert(visible_machines==1);
    model.machines[0].state=RUNNING;
    tap(145,130); assert(machine_filter==FILTER_ALL && visible_machines==19);
    lv_obj_scroll_to_y(machine_list,500,LV_ANIM_OFF);
    assert(lv_obj_get_scroll_y(machine_list)==500);
    refresh_data(1); assert(lv_obj_get_scroll_y(machine_list)==500);
    lv_obj_scroll_to_y(machine_list,839,LV_ANIM_OFF);
    capture(dir,"real-final-lista");
    home(); assert(machine_scroll==0);
    model.host_state=HOST_ERROR; stream.last_valid=ticks()-12000;
    show(SUMMARY); capture(dir,"real-error-api");
    puts("PANEL_SELF_CHECK_OK: navigation, missing data, Home, gestures, mocked power");
    return 0;
}

#ifdef __linux__
static int data_fd=-1;
static uint32_t last_open;
static void read_data(void)
{
    uint32_t now=ticks();
    if(data_fd<0 && (uint32_t)(now-last_open)>=1000) {
        last_open=now;
        data_fd=open("/dev/ttyGS1",O_RDWR|O_NONBLOCK|O_NOCTTY);
        if(data_fd>=0) {
            struct termios attrs;
            if(tcgetattr(data_fd,&attrs)<0) { close(data_fd); data_fd=-1; }
            else {
                cfmakeraw(&attrs); attrs.c_cflag|=CLOCAL|CREAD;
                cfsetispeed(&attrs,B115200); cfsetospeed(&attrs,B115200);
                if(tcsetattr(data_fd,TCSANOW,&attrs)<0) { close(data_fd); data_fd=-1; }
            }
        }
    }
    int changed=0;
    unsigned before=stream.valid;
    if(data_fd>=0) {
        struct pollfd p={.fd=data_fd,.events=POLLIN};
        if(poll(&p,1,0)>0) {
            char buf[4096];
            ssize_t n=(p.revents&POLLIN)?read(data_fd,buf,sizeof(buf)):-1;
            if(n>0) changed=panel_stream_feed(&stream,buf,(size_t)n,now,&model);
            if((p.revents&(POLLHUP|POLLERR|POLLNVAL)) ||
               (n<0 && (p.revents&POLLIN) && errno!=EAGAIN && errno!=EINTR)) {
                close(data_fd); data_fd=-1; stream.used=0; stream.dropping=0;
            }
        }
    }
    panel_stream_expire(&stream,now);
    if(before!=stream.valid) printf("DATA_VALID samples=%u cpu=%d host=%s\n",stream.valid,model.cpu_percent,model.host);
    static int old_state=-1;
    int state=!stream.valid?0:model.host_state==HOST_ERROR?3:panel_stream_stale(&stream,now)?2:1;
    if(state!=old_state) { printf("DATA_STATE %s\n",state==0?"waiting":state==1?"fresh":state==2?"stale":"api-error"); old_state=state; }
    static unsigned old_rejected;
    static uint32_t last_report;
    if(stream.rejected!=old_rejected && (uint32_t)(now-last_report)>=1000) {
        printf("DATA_REJECTED total=%u\n",stream.rejected);
        old_rejected=stream.rejected; last_report=now;
    }
    refresh_data(changed);
}

static int find_input(const char *expected, char *path, size_t size)
{
    for(int i=0;i<32;i++) {
        snprintf(path,size,"/dev/input/event%d",i);
        int fd=open(path,O_RDONLY|O_NONBLOCK);
        if(fd<0) continue;
        char name[128]={0};
        if(ioctl(fd,EVIOCGNAME(sizeof(name)),name)>=0 && !strcmp(name,expected)) return fd;
        close(fd);
    }
    return -1;
}

static int live(void)
{
    char path[64];
    int keys=find_input("1c22800.lradc",path,sizeof(path));
    if(keys<0) { fputs("Home input not found\n",stderr); return 1; }
    int input=find_input("silead_ts",path,sizeof(path));
    if(input<0) { close(keys); fputs("Touch input not found\n",stderr); return 1; }
    int fb=open("/dev/fb0",O_RDWR);
    struct fb_var_screeninfo v;
    if(fb<0 || ioctl(fb,FBIOGET_VSCREENINFO,&v)<0 || v.xres!=1024 || v.yres!=600 ||
       v.bits_per_pixel!=32 || v.red.offset!=16 || v.green.offset!=8 || v.blue.offset!=0) {
        fputs("Framebuffer layout not supported; no UI started\n",stderr);
        if(fb>=0) close(fb);
        close(input); close(keys); return 1;
    }
    close(fb);
    int tty=open("/dev/tty0",O_RDWR);
    if(tty<0 || ioctl(tty,KDSETMODE,KD_GRAPHICS)<0) {
        if(tty>=0) close(tty);
        close(input); close(keys); return 1;
    }
    lv_display_t *d=lv_linux_fbdev_create();
    lv_linux_fbdev_set_file(d,"/dev/fb0");
    touch=lv_evdev_create_fd(LV_INDEV_TYPE_POINTER,input);
    if(!touch) { ioctl(tty,KDSETMODE,KD_TEXT); close(tty); close(keys); return 1; }
    // shortcut: preserve the POC's approximate Y/650 mapping until physical calibration.
    lv_evdev_set_calibration(touch,0,0,1023,649);
    root=lv_screen_active(); lv_obj_set_style_bg_color(root,lv_color_hex(bg),0);
    lv_obj_set_style_bg_opa(root,LV_OPA_COVER,0);
    usb_mode=1;
    memset(&model,0,sizeof(model)); model.cpu_percent=-1;
    strcpy(model.host,"Esperando host");
    show(SUMMARY); puts("PANEL_UI_READY");
    while(!stopping) {
        struct input_event e;
        ssize_t n;
        while((n=read(keys,&e,sizeof(e)))==(ssize_t)sizeof(e))
            if(e.type==EV_KEY && e.code==KEY_HOME && e.value==0) home();
        if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK && errno!=EINTR) break;
        if(power_child>0) {
            int status;
            pid_t result=waitpid(power_child,&status,WNOHANG);
            if(result==power_child) {
                power_child=0;
                if(!WIFEXITED(status) || WEXITSTATUS(status)!=0) {
                    sent=0; show(SYSTEM);
                    label(root,32,492,960,"Fallo la solicitud de energia. HOME para volver.",
                          &lv_font_montserrat_20,orange);
                }
            }
        }
        read_data(); lv_timer_handler(); usleep(5000);
    }
    lv_evdev_delete(touch); close(keys);
    if(data_fd>=0) close(data_fd);
    ioctl(tty,KDSETMODE,KD_TEXT); close(tty);
    return stopping?0:1;
}
#endif

int main(int argc, char **argv)
{
    setvbuf(stdout,NULL,_IOLBF,0);
    signal(SIGINT,stop); signal(SIGTERM,stop);
    lv_init(); lv_tick_set_cb(ticks);
    if(argc==3 && !strcmp(argv[1],"--preview")) return preview_all(argv[2]);
#ifdef __linux__
    if(argc==2 && !strcmp(argv[1],"--ui")) return live();
#endif
    fputs("Usage: tablet-panel --preview EXISTING_DIR | --ui (Linux only)\n",stderr);
    return 2;
}
