// SPDX-License-Identifier: GPL-2.0-or-later
#include "protocol.h"
#include "vendor/cJSON.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static PanelData baseline;
static void reject(const char *s)
{
    PanelData out=baseline;
    assert(!panel_parse(s,strlen(s),&out));
    assert(!memcmp(&out,&baseline,sizeof(out)));
}
static void reject_object(cJSON *o)
{
    char *s=cJSON_PrintUnformatted(o); assert(s); reject(s); cJSON_free(s); cJSON_Delete(o);
}
static void replace_and_reject(const char *s, const char *old, const char *value)
{
    const char *p=strstr(s,old); assert(p);
    char buf[8192];
    assert(snprintf(buf,sizeof(buf),"%.*s%s%s",(int)(p-s),s,value,p+strlen(old))<(int)sizeof(buf));
    reject(buf);
}
int main(int argc, char **argv)
{
    assert(argc==2);
    FILE *f=fopen(argv[1],"r"); assert(f);
    char fixture[4097]={0}; size_t n=fread(fixture,1,4096,f); fclose(f);
    assert(n>0 && n<4096 && panel_parse(fixture,n,&baseline));
    assert(baseline.cpu_percent==23 && baseline.count==5 && baseline.host_state==HOST_OK);
    assert(baseline.machines[3].state==STOPPED && baseline.machines[4].state==UNKNOWN);
    cJSON *o=cJSON_Parse(fixture); assert(o);
    char *wire=cJSON_PrintUnformatted(o); assert(wire); cJSON_Delete(o);
    size_t len=strlen(wire);
    const char *bad_cpu[]={"-1","101","1.5","true","\"23\"","1e999","01","1.","1e","NaN"};
    // Each numeric edge is tested as a complete message, not just a JSON primitive.
    for(size_t i=0;i<sizeof(bad_cpu)/sizeof(*bad_cpu);i++) {
        char value[128]; snprintf(value,sizeof(value),"\"cpu_pct\":%s",bad_cpu[i]);
        replace_and_reject(wire,"\"cpu_pct\":23",value);
    }
    replace_and_reject(wire,"\"v\":1","\"v\":1,\"v\":1");
    replace_and_reject(wire,"\"v\":1","\"v\":2");
    replace_and_reject(wire,"\"demo\":true","\"demo\":false");
    replace_and_reject(wire,"pve-homelab","bad\\u0000host");
    replace_and_reject(wire,"pve-homelab","bad\\nhost");
    replace_and_reject(wire,"pve-homelab","bad\thost");
    replace_and_reject(wire,"pve-homelab","abcdefghijklmnopqrstuvwxyz0123456789");
    replace_and_reject(wire,"\"kind\":\"VM\"","\"kind\":\"XX\"");
    replace_and_reject(wire,"\"state\":\"running\"","\"state\":\"typo\"");
    replace_and_reject(wire,"\"host_state\":\"ok\"","\"host_state\":null");
    replace_and_reject(wire,"\"id\":101","\"id\":100");
    o=cJSON_Parse(wire); cJSON_DeleteItemFromObjectCaseSensitive(o,"cpu_pct"); reject_object(o);
    o=cJSON_Parse(wire); cJSON_AddBoolToObject(o,"extra",1); reject_object(o);
    o=cJSON_Parse(wire); cJSON_ReplaceItemInObjectCaseSensitive(o,"ram_total_bytes",cJSON_CreateNumber(1)); reject_object(o);
    o=cJSON_Parse(wire); cJSON_ReplaceItemInObjectCaseSensitive(o,"ram_total_bytes",cJSON_CreateNull()); reject_object(o);
    o=cJSON_Parse(wire); cJSON *list=cJSON_GetObjectItemCaseSensitive(o,"machines");
    cJSON_AddItemToArray(list,cJSON_Duplicate(list->child,1)); reject_object(o);
    reject("{}"); reject("[]"); reject("{\"a\":[[[[[[[[[[0]]]]]]]]]]}");
    char tail[8192]; snprintf(tail,sizeof(tail),"%s garbage",wire); reject(tail);
    PanelData out=baseline;
    assert(!panel_parse(wire,len-1,&out));
    o=cJSON_Parse(wire);
    cJSON_ReplaceItemInObjectCaseSensitive(o,"cpu_pct",cJSON_CreateNull());
    const char *nulls[]={"ram_used_bytes","ram_total_bytes","disk_used_bytes","disk_total_bytes"};
    for(int i=0;i<4;i++) cJSON_ReplaceItemInObjectCaseSensitive(o,nulls[i],cJSON_CreateNull());
    char *missing=cJSON_PrintUnformatted(o); assert(panel_parse(missing,strlen(missing),&out));
    assert(out.cpu_percent==-1 && out.ram_total==0 && out.disk_total==0);
    cJSON_free(missing); cJSON_Delete(o);
    o=cJSON_Parse(wire); cJSON_ReplaceItemInObjectCaseSensitive(o,"cpu_pct",cJSON_CreateNumber(0));
    char *zero=cJSON_PrintUnformatted(o); assert(panel_parse(zero,strlen(zero),&out));
    assert(out.cpu_percent==0); cJSON_Delete(o); cJSON_free(zero);

    char framed[4098]; snprintf(framed,sizeof(framed),"%s\n",wire);
    PanelStream s={0}; out=baseline;
    panel_stream_feed(&s,framed,5,100,&out); assert(!s.valid);
    panel_stream_feed(&s,framed+5,len+1-5,101,&out); assert(s.valid==1 && s.last_valid==101);
    assert(!panel_stream_stale(&s,10100) && panel_stream_stale(&s,10101));
    panel_stream_feed(&s,"{}\n",3,200,&out); assert(s.rejected==1 && s.last_valid==101);
    panel_stream_feed(&s,framed,5,1000,&out); panel_stream_expire(&s,4000);
    panel_stream_feed(&s,framed+5,len+1-5,4001,&out); assert(s.valid==1 && s.dropping==0);
    char huge[PANEL_MAX_LINE+100]; memset(huge,'x',sizeof(huge));
    panel_stream_feed(&s,huge,sizeof(huge),5000,&out); assert(s.dropping && s.used==0);
    panel_stream_feed(&s,"\n",1,5001,&out);
    panel_stream_feed(&s,framed,len+1,5002,&out); assert(s.valid==2 && s.last_valid==5002);
    s.last_valid=UINT32_MAX-100;
    assert(!panel_stream_stale(&s,50) && panel_stream_stale(&s,10000));
    panel_stream_feed(&s,"a\0b\n",4,10001,&out); assert(s.valid==2);
    o=cJSON_Parse(wire);
    cJSON_ReplaceItemInObjectCaseSensitive(o,"v",cJSON_CreateNumber(2));
    cJSON_ReplaceItemInObjectCaseSensitive(o,"demo",cJSON_CreateFalse());
    cJSON_AddStringToObject(o,"storage","local-lvm");
    list=cJSON_GetObjectItemCaseSensitive(o,"machines");
    for(int i=5;i<128;i++) {
        cJSON *m=cJSON_Duplicate(list->child,1);
        cJSON_ReplaceItemInObjectCaseSensitive(m,"id",cJSON_CreateNumber(100+i));
        cJSON_AddItemToArray(list,m);
    }
    char *real=cJSON_PrintUnformatted(o);
    assert(panel_parse(real,strlen(real),&out));
    assert(out.count==128 && !out.demo && !strcmp(out.storage,"local-lvm"));
    char real_frame[PANEL_MAX_LINE+2]; snprintf(real_frame,sizeof(real_frame),"%s\n",real);
    panel_stream_feed(&s,real_frame,strlen(real_frame),15000,&out);
    unsigned samples=s.samples; uint32_t last=s.last_valid;
    cJSON_ReplaceItemInObjectCaseSensitive(o,"host_state",cJSON_CreateString("error"));
    cJSON_ReplaceItemInObjectCaseSensitive(o,"cpu_pct",cJSON_CreateNull());
    char *error=cJSON_PrintUnformatted(o);
    snprintf(real_frame,sizeof(real_frame),"%s\n",error);
    panel_stream_feed(&s,real_frame,strlen(real_frame),20000,&out);
    assert(out.host_state==HOST_ERROR && out.cpu_percent==23 && out.count==128);
    assert(s.samples==samples && s.last_valid==last);
    cJSON *extra=cJSON_Duplicate(list->child,1);
    cJSON_ReplaceItemInObjectCaseSensitive(extra,"id",cJSON_CreateNumber(999));
    cJSON_AddItemToArray(list,extra); reject_object(o);
    cJSON_free(error); cJSON_free(real);
    unsigned seed=12345;
    for(int i=0;i<10000;i++) {
        char mutation[4098]; memcpy(mutation,framed,len+1);
        seed=seed*1664525u+1013904223u;
        mutation[seed%len]=(char)(seed>>24);
        panel_stream_feed(&s,mutation,len+1,(uint32_t)i+20000,&out);
        assert(s.used<=PANEL_MAX_LINE);
    }
    cJSON_free(wire);
    puts("PROTOCOL_SELF_CHECK_OK: strict schema, atomic rejection, framing, timeout, stale, recovery, mutations");
    return 0;
}
