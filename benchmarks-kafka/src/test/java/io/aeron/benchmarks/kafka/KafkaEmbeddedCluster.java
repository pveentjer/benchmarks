/*
 * Copyright 2015-2025 Real Logic Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package io.aeron.benchmarks.kafka;

import io.aeron.benchmarks.Configuration;
import kafka.server.KafkaConfig;
import kafka.server.KafkaRaftServer;
import org.apache.kafka.common.Uuid;
import org.apache.kafka.common.utils.Time;
import org.apache.kafka.metadata.properties.MetaProperties;
import org.apache.kafka.metadata.properties.MetaPropertiesVersion;

import java.io.BufferedWriter;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Properties;

class KafkaEmbeddedCluster implements AutoCloseable
{
    private final Path logDir;
    private final KafkaRaftServer kafka;

    KafkaEmbeddedCluster(final int httpPort, final int sslPort, final int controllerPort, final Path tempDir)
        throws Exception
    {
        logDir = tempDir.resolve("log-dir");
        Files.createDirectory(logDir);

        final int nodeId = 1;
        final KafkaConfig config = createConfig(nodeId, httpPort, sslPort, controllerPort);

        final Path confFile = logDir.resolve("meta.properties");
        final Properties metaProperties = new MetaProperties.Builder()
            .setClusterId(Uuid.randomUuid().toString())
            .setVersion(MetaPropertiesVersion.V1)
            .setNodeId(nodeId)
            .setDirectoryId(Uuid.randomUuid())
            .build()
            .toProperties();
        try (BufferedWriter writer = Files.newBufferedWriter(confFile))
        {
            metaProperties.store(writer, null);
        }

        kafka = new KafkaRaftServer(config, Time.SYSTEM);
        kafka.startup();
    }

    private KafkaConfig createConfig(final int nodeId, final int httpPort, final int sslPort, final int controllerPort)
    {
        final Properties props = new Properties();

        props.put("process.roles", "broker,controller");
        props.put("controller.quorum.voters", "1@localhost:" + controllerPort);
        props.put("controller.listener.names", "CONTROLLER");
        props.put("listeners",
            "PLAINTEXT://localhost:" + httpPort +
            ",SSL://localhost:" + sslPort +
            ",CONTROLLER://localhost:" + controllerPort);
        props.put("listener.security.protocol.map",
            "CONTROLLER:PLAINTEXT,PLAINTEXT:PLAINTEXT,SSL:SSL,SASL_PLAINTEXT:SASL_PLAINTEXT,SASL_SSL:SASL_SSL");
        props.put("node.id", Integer.toString(nodeId));
        props.put("broker.id", Integer.toString(nodeId));
        props.put("log.dir", logDir.toAbsolutePath().toString());
        props.put("advertised.listeners",
            "PLAINTEXT://localhost:" + httpPort + ",SSL://localhost:" + sslPort);
        final Path certificatesPath = Configuration.tryResolveCertificatesDirectory();
        props.put("ssl.truststore.location",
            certificatesPath.resolve("truststore.p12").toString());
        props.put("ssl.truststore.type", "PKCS12");
        props.put("ssl.truststore.password", "truststore");
        props.put("ssl.keystore.location",
            certificatesPath.resolve("server.keystore").toString());
        props.put("ssl.keystore.type", "PKCS12");
        props.put("ssl.keystore.password", "server");
        props.put("ssl.client.auth", "required");
        props.put("auto.create.topics.enable", "true");
        props.put("message.max.bytes", "1000000");
        props.put("controlled.shutdown.enable", "true");
        props.put("log.message.downconversion.enable", "false");
        props.put("num.partitions", "1");
        props.put("default.replication.factor", "1");
        props.put("offsets.topic.replication.factor", "1");
        props.put("num.network.threads", "1");
        props.put("num.io.threads", "1");
        props.put("background.threads", "1");
        props.put("log.cleaner.threads", "1");
        props.put("num.recovery.threads.per.data.dir", "1");
        props.put("num.replica.alter.log.dirs.threads", "1");

        return new KafkaConfig(props);
    }

    public void close()
    {
        kafka.shutdown();
        kafka.awaitShutdown();
    }
}
