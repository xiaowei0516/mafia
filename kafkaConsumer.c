//
// Created by tyy on 2017/1/9.
//
#include <ctype.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <syslog.h>
#include <sys/time.h>
#include <errno.h>
#include <getopt.h>

#include "kafkaConsumer.h"


static void stop (int sig) {
    if (!kconf.run) {
        exit(1);
    }
    kconf.run = 0;
}


/**
 * Kafka logger callback (optional)
 */
static void logger (const rd_kafka_t *rk, int level,
        const char *fac, const char *buf) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    log(KLOG_INFO, "%u.%03u RDKAFKA-%i-%s: %s: %s\n",
            (int)tv.tv_sec, (int)(tv.tv_usec / 1000),
            level, fac, rd_kafka_name(rk), buf);
}


/**
 * Handle and print a consumed message.
 * Internally crafted messages are also used to propagate state from
 * librdkafka to the application. The application needs to check
 * the `rkmessage->err` field for this purpose.
 */
static void msg_consume (rd_kafka_message_t *rkmessage,
        void *opaque) {
    if (kconf.run == 0) {
        return;
    }

    if (rkmessage->err) {
        if (rkmessage->err == RD_KAFKA_RESP_ERR__PARTITION_EOF) {
            log(KLOG_WAR, "%% Consumer reached end of %s [%d] "
                    "message queue at offset %ld\n",
                    rd_kafka_topic_name(rkmessage->rkt),
                    rkmessage->partition, rkmessage->offset);
            return;
        }

        if (rkmessage->rkt) {
            log_err("%% Consume error for "
                    "topic \"%s\" [%d] "
                    "offset %ld: %s\n",
                    rd_kafka_topic_name(rkmessage->rkt),
                    rkmessage->partition,
                    rkmessage->offset,
                    rd_kafka_message_errstr(rkmessage));
        } else {
            log_err("%% Consumer error: %s: %s\n",
                    rd_kafka_err2str(rkmessage->err),
                    rd_kafka_message_errstr(rkmessage));
        }

        if (rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_PARTITION ||
                rkmessage->err == RD_KAFKA_RESP_ERR__UNKNOWN_TOPIC) {
            kconf.run = 0;
        }

        return;
    }

    log(KLOG_DEBUG, "%% Message (topic %s [%d], "
            "offset %ld, %zd bytes):\n",
            rd_kafka_topic_name(rkmessage->rkt),
            rkmessage->partition,
            rkmessage->offset, rkmessage->len);

    if (rkmessage->key_len) {
        log(KLOG_DEBUG, "Key: %.*s\n", (int)rkmessage->key_len, (char *)rkmessage->key);
    }

    if (kconf.payload_cb) {
        kconf.payload_cb(rkmessage);
    } else {
        log(KLOG_DEBUG, "%.*s\n", (int)rkmessage->len, (char *)rkmessage->payload);
    }
}


static void print_partition_list (const rd_kafka_topic_partition_list_t
        *partitions) {
    int i;
    for (i = 0 ; i < partitions->cnt ; i++) {
        fprintf(stderr, "%s topic: %s[%d] offset %ld",
                i > 0 ? ",":"",
                partitions->elems[i].topic,
                partitions->elems[i].partition,
                partitions->elems[i].offset);
    }
    fprintf(stderr, "\n");

}


static RD_UNUSED void set_partition_offset (rd_kafka_topic_partition_list_t
        *partitions, const int64_t offset) {
    int i;
    for (i = 0 ; i < partitions->cnt ; i++) {
        rd_kafka_topic_partition_t *part;
        char *topic = partitions->elems[i].topic;
        int32_t partition = partitions->elems[i].partition;
        if ((part = rd_kafka_topic_partition_list_find(partitions, topic, partition))) {
            part->offset = offset;
        } else {
            log_err("set partition offset error: %s[%d] offset:%ld\n",
                    topic, partition, offset);
        }
    }
}


static void rebalance_cb (rd_kafka_t *rk,
        rd_kafka_resp_err_t err,
        rd_kafka_topic_partition_list_t *partitions,
        void *opaque) {
    fprintf(stderr, "%% Consumer group rebalanced - ");

    switch (err)  {
        case RD_KAFKA_RESP_ERR__ASSIGN_PARTITIONS:
            fprintf(stderr, "assigned:\n");

            set_partition_offset(partitions, kconf.offset);

            print_partition_list(partitions);
            rd_kafka_assign(rk, partitions);
            break;

        case RD_KAFKA_RESP_ERR__REVOKE_PARTITIONS:
            fprintf(stderr, "revoked:\n");
            print_partition_list(partitions);
            rd_kafka_assign(rk, NULL);
            break;

        default:
            log_err("failed: %s\n", rd_kafka_err2str(err));
            rd_kafka_assign(rk, NULL);
            break;
    }
}


//int main (int argc, char **argv) {
int init_kafka_consumer(void) {
    int i;
    char tmp[16];
    char errstr[512];
    rd_kafka_resp_err_t err;

    /* Kafka configuration */
    kconf.rk_conf = rd_kafka_conf_new();

    /* Set logger */
    rd_kafka_conf_set_log_cb(kconf.rk_conf, logger);

    /* Quick termination */
    snprintf(tmp, sizeof(tmp), "%i", SIGIO);
    rd_kafka_conf_set(kconf.rk_conf, "internal.termination.signal", tmp, NULL, 0);

    /* Topic configuration */
    kconf.rkt_conf = rd_kafka_topic_conf_new();

    signal(SIGINT, stop);

    /* Consumer groups require a group id */
    if (rd_kafka_conf_set(kconf.rk_conf, "group.id", kconf.group,
                errstr, sizeof(errstr)) !=
            RD_KAFKA_CONF_OK) {
        log_err("%% %s\n", errstr);
        exit(1);
    }

    /* Set default topic config for pattern-matched topics. */
    rd_kafka_conf_set_default_topic_conf(kconf.rk_conf, kconf.rkt_conf);

    /* Callback called on partition assignment changes */
    rd_kafka_conf_set_rebalance_cb(kconf.rk_conf, rebalance_cb);

    /* Create Kafka handle */
    if (!(kconf.rk = rd_kafka_new(RD_KAFKA_CONSUMER, kconf.rk_conf,
                    errstr, sizeof(errstr)))) {
        log_err("%% Failed to create new consumer: %s\n", errstr);
        exit(1);
    }

    rd_kafka_set_log_level(kconf.rk, LOG_DEBUG);

    /* Add brokers */
    if (rd_kafka_brokers_add(kconf.rk, kconf.brokers) == 0) {
        log_err("%% No valid brokers specified\n");
        exit(1);
    }

    /* Redirect rd_kafka_poll() to consumer_poll() */
    rd_kafka_poll_set_consumer(kconf.rk);

    // TODO: just support one topic, fix it according to the parameter
    kconf.rktp = rd_kafka_topic_partition_list_new(kconf.topic_count);
    for (i = 0; i < kconf.topic_count; i++) {
        rd_kafka_topic_partition_list_add(kconf.rktp, kconf.topic[i], kconf.partition);
    }

    log(KLOG_INFO, "%% Subscribing to %d topics\n", kconf.rktp->cnt);

    if ((err = rd_kafka_subscribe(kconf.rk, kconf.rktp))) {
        log_err("%% Failed to start consuming topics: %s\n",
                rd_kafka_err2str(err));
        exit(1);
    }
    rd_kafka_topic_partition_list_destroy(kconf.rktp);

    while (kconf.run) {
        rd_kafka_message_t *rkmessage;

        rkmessage = rd_kafka_consumer_poll(kconf.rk, 1000);
        if (rkmessage) {
            msg_consume(rkmessage, NULL);
            rd_kafka_message_destroy(rkmessage);
        }
    }

    err = rd_kafka_consumer_close(kconf.rk);
    if (err) {
        log_err("%% Failed to close consumer: %s\n", rd_kafka_err2str(err));
    } else {
        fprintf(stderr, "%% Consumer closed\n");
    }

    /* Destroy handle */
    rd_kafka_destroy(kconf.rk);

    /* Let background threads clean up and terminate cleanly. */
    kconf.run = 5;
    while (kconf.run-- > 0 && rd_kafka_wait_destroyed(1000) == -1) {
        log(KLOG_INFO, "Waiting for librdkafka to decommission\n");
    }
    if (kconf.run <= 0) {
        rd_kafka_dump(stdout, kconf.rk);
    }

    return 0;
}
