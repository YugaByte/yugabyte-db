[Yugastore-java](https://github.com/yugabyte/yugastore-java) app is an end-to-end ecommerce application built using a microservices design pattern. React UI, Spring Boot app framework and YugabyteDB (both YSQL and YCQL) are used as the underlying technology stack.

## 1. Create cluster

Create a cluster.

```sh
$ ./bin/yb-ctl create
``` 
Clients can now connect to the YSQL API at `localhost:5433` and YCQL API at `localhost:9042`.

## 2. Install Yugastore

Clone the repo.
```sh
$ git clone https://github.com/yugabyte/yugastore-java.git
```
```sh
$ cd yugastore-java
```

Create the app binaries.
```sh
$ mvn -DskipTests package
```

Create the app's schema in YugabyteDB.

```sh
$ cd resources
```

```sh
$ $YUGABYTE_HOME/bin/ysqlsh -f schema.sql
```

```sh
$ $YUGABYTE_HOME/bin/ycqlsh -f schema.cql
```

Load the initial data.

```sh
$ ./dataload.sh
```

## 3. Start the app

Start the Eureka discovery service. After starting, verify that the service is running at `http://localhost:8761/`.

```sh
$ cd eureka-server-local/ && mvn spring-boot:run
```

Start the API gateway microservice.

```sh
$ cd api-gateway-microservice/ && mvn spring-boot:run
```

Start the products microservice.

```sh
$ cd products-microservice/ && mvn spring-boot:run
```

Start the checkout microservice.

```sh
$ cd checkout-microservice/ && mvn spring-boot:run
```

Start the UI service. After starting, browse the app at http://localhost:8080/.

```sh
$ cd react-ui/ && mvn spring-boot:run
```

## 4. Add items to the cart

Add two items to the cart as shown below.

![yugastore-java checkout](/images/quick_start/binary-yugastore-java-checkout.png)

Verify that your cart is now stored inside the YSQL `shopping_cart` table. From your YugabyteDB local cluster home, run the following.

```sh
$ ./bin/ysqlsh
```

```sh
yugabyte=# select * from shopping_cart;
```

```
 cart_key     | user_id |    asin    |       time_added        | quantity 
------------------+---------+------------+-------------------------+----------
 u1001-0001048236 | u1001   | 0001048236 | 2019-05-29T13:46:54.046 |        1
 u1001-0001048775 | u1001   | 0001048775 | 2019-05-29T13:46:56.055 |        1
(2 rows)
```

## 5. Verify the completed order

Now complete the checkout and observe the order number generated. 

![yugastore-java order confirmation](/images/quick_start/binary-yugastore-java-orderconfirmation.png)

Verify that this order number is now in the YCQL `orders` table.

```sh
$ ./bin/ycqlsh localhost
```

```sh
ycqlsh> select * from cronos.orders;
```

```
order_id                             | user_id | order_details                                                                                                                                                                           | order_time              | order_total
--------------------------------------+---------+-----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------+-------------------------+-------------
 119b96dc-7d1a-4f2f-a1dd-36def25348e0 |       1 | Customer bought these Items:  Product: The Sherlock Holmes Audio Collection, Quantity: 1; Product: Measure for Measure: Complete &amp; Unabridged, Quantity: 1; Order Total is : 741.95 | 2019-05-29T13:54:04.093 |      741.95

```

Verify that there are no active shopping carts in YSQL at this point.

```sh
yugabyte=# select * from shopping_cart;
```

```
cart_key | user_id | asin | time_added | quantity 
----------+---------+------+------------+----------
(0 rows)
```

## 6. Run IoT Fleet Management app

After running Yugastore, Yugabyte recommends running the [IoT Fleet Management](../realworld-apps/iot-spark-kafka-ksql/) app. This app is built on top of YugabyteDB as the database (using the YCQL API), Confluent Kafka as the message broker, KSQL or Apache Spark Streaming for real-time analytics and Spring Boot as the application framework.
