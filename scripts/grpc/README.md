Overview
--------

This directory provides scripts to benchmark gRPC using the `remote.io.aeron.benchmarks.LoadTestRig`.

All benchmarks require two nodes: "client" and "server", where "client" is the
`remote.io.aeron.benchmarks.LoadTestRig` that uses one of the
`io.aeron.benchmarks.message.transceiver` implementations under the hood, and the "server" is the
remote node that pipes messages through.

NOTE: It is advised to have "client" and "server" run on different machines.

Test scenarios
--------------

1. Echo benchmark using streaming client.

    Start the scripts in the following order: `server` -> `client`.


Configuration
-------------
* `io.aeron.benchmarks.grpc.server.host` - server host. Default value is `localhost`.
* `io.aeron.benchmarks.grpc.server.port` - server port number. Default value is `13400`.
* `io.aeron.benchmarks.grpc.tls` - should the TLS be used for client/server communication.
Default value is `false`.


Overriding properties
---------------------

There are three ways to define and/or override properties:

1. Create a file named `benchmark.properties` and define your properties there.

    For example:
    ```
    io.aeron.benchmarks.grpc.server.host=127.0.0.1
    io.aeron.benchmarks.grpc.server.port=13400
    io.aeron.benchmarks.grpc.tls=true
    ```

1. Supply custom properties file(s) as the last argument to a script, e.g.:

    ```
    ./blocking-client my-custom-props.properties
    ```

1. Set system properties via `JVM_OPTS` environment variable, e.g.:

    ```
    export JVM_OPTS="${JVM_OPTS} -Dio.aeron.benchmarks.grpc.server.host=192.168.10.10"
    
    ./server
    ```
