/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * The following only applies to changes made to this file as part of YugaByte development.
 *
 *     Portions Copyright (c) YugaByte, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"); you may not use this file
 * except in compliance with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software distributed under the
 * License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied.  See the License for the specific language governing permissions
 * and limitations under the License.
 */
package org.yb.cql;

import org.junit.Before;
import org.junit.Test;
import org.junit.runner.RunWith;

import org.yb.YBTestRunner;

@RunWith(value=YBTestRunner.class)
public class TestExplainPlan extends CQLTester {

  @Before
  public void setUpTable() throws Exception {
    createKeyspace("imdb");
    useKeyspace("imdb");
    createTable("CREATE TABLE movie_stats (\n" +
      "       movie_name text,\n" +
      "       movie_genre text,\n" +
      "       user_name text,\n" +
      "       user_rank int,\n" +
      "       last_watched timestamp,\n" +
      "       PRIMARY KEY (movie_genre, movie_name, user_name)\n" +
      ") WITH transactions = { 'enabled' : true };");
    createIndex("CREATE INDEX IF NOT EXISTS most_watched_by_year\n" +
      "  ON movie_stats((movie_genre, last_watched), movie_name, user_name)\n" +
      "  INCLUDE(user_rank);");
    createIndex("CREATE INDEX IF NOT EXISTS best_rated\n" +
      "  ON movie_stats((user_rank, movie_genre), movie_name, user_name);");
    execute("INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank,\n"
      +" last_watched) VALUES ('m1', 'g1', 'u1', 5, '2019-01-18');");
    execute("INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank,\n"
      +" last_watched) VALUES ('m2', 'g2', 'u1', 4, '2019-01-17');");
    execute("INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank,\n"
      +" last_watched) VALUES ('m3', 'g1', 'u2', 5, '2019-01-18');");
    execute("INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank,\n"
      +" last_watched) VALUES ('m4', 'g1', 'u1', 2, '2019-02-27');");
  }

  @Test
  public void testSelectPlan() throws Exception {
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre = 'g1';",
      "Row[Range Scan on imdb.movie_stats        ]" +
      "Row[  Key Conditions: (movie_genre = 'g1')]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre = 'g1'\n" +
      "and last_watched='2019-02-27';",
      "Row[Index Only Scan using imdb.most_watched_by_year on imdb.movie_stats     ]" +
      "Row[  Key Conditions: (movie_genre = 'g1') AND (last_watched = '2019-02-27')]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre = 'g2'and user_rank=5;",
      "Row[Index Scan using imdb.best_rated on imdb.movie_stats      ]" +
      "Row[  Key Conditions: (user_rank = 5) AND (movie_genre = 'g2')]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats where movie_genre in ('g1', 'g2');",
      "Row[Range Scan on imdb.movie_stats         ]" +
      "Row[  Key Conditions: (movie_genre IN expr)]");
    assertQuery("EXPLAIN SELECT COUNT(*) FROM movie_stats WHERE movie_genre = 'g2'and user_rank=5;",
      "Row[Aggregate                                                       ]" +
      "Row[  ->  Index Only Scan using imdb.best_rated on imdb.movie_stats ]" +
      "Row[        Key Conditions: (user_rank = 5) AND (movie_genre = 'g2')]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre='g2'\n" +
      "and movie_name='m2' LIMIT 5;",
      "Row[Limit                                       ]" +
      "Row[  ->  Range Scan on imdb.movie_stats        ]" +
      "Row[        Key Conditions: (movie_genre = 'g2')]" +
      "Row[        Filter: (movie_name = 'm2')         ]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE user_rank = 5 AND movie_genre = 'm2' " +
      "LIMIT 1;",
      "Row[Limit                                                           ]" +
      "Row[  ->  Index Scan using imdb.best_rated on imdb.movie_stats      ]" +
      "Row[        Key Conditions: (user_rank = 5) AND (movie_genre = 'm2')]");
    assertQuery("EXPLAIN SELECT user_name FROM movie_stats WHERE user_rank = 5 AND " +
      "movie_genre = 'm2' LIMIT 1;",
      "Row[Limit                                                           ]" +
      "Row[  ->  Index Only Scan using imdb.best_rated on imdb.movie_stats ]" +
      "Row[        Key Conditions: (user_rank = 5) AND (movie_genre = 'm2')]");
  }

  @Test
  public void testSelectPlanWithTokens() throws Exception {
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE token(movie_genre) >= ? " +
        "and partition_hash(movie_genre) <= :genre;",
      "Row[Range Scan on imdb.movie_stats                                " +
        "                                            ]" +
      "Row[  Key Conditions: (token(movie_genre) >= :partition key token)" +
        " AND (partition_hash(movie_genre) <= :genre)]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE partition_hash(movie_genre) >= 3" +
        " and token(movie_genre) <= token('g1')",
      "Row[Range Scan on imdb.movie_stats                       " +
        "                                     ]" +
      "Row[  Key Conditions: (partition_hash(movie_genre) >= 3) " +
        "AND (token(movie_genre) <= token(g1))]");
  }

  @Test
  public void testSelectPlanWithBindVars() throws Exception {
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre = ? and user_rank = :5;",
      "Row[Index Scan using imdb.best_rated on imdb.movie_stats               ]" +
      "Row[  Key Conditions: (user_rank = :5) AND (movie_genre = :movie_genre)]");
    assertQuery("EXPLAIN SELECT * FROM movie_stats WHERE movie_genre= :1\n" +
        "and movie_name= :name",
        "Row[Range Scan on imdb.movie_stats      ]" +
        "Row[  Key Conditions: (movie_genre = :1)]" +
        "Row[  Filter: (movie_name = :name)      ]");
  }

  @Test
  public void testInsertPlan() throws Exception {
    assertQuery("EXPLAIN INSERT INTO movie_stats(movie_name, movie_genre, user_name, user_rank,\n" +
      " last_watched) VALUES ('m4', 'g1', 'u1', 2, '2019-02-27');",
      "Row[Insert on imdb.movie_stats]");
  }

  @Test
  public void testDeletePlan() throws Exception {
    assertQuery("EXPLAIN DELETE FROM movie_stats WHERE movie_genre = 'g1'and movie_name='m1';",
      "Row[Delete on imdb.movie_stats                  ]" +
      "Row[  ->  Range Scan on imdb.movie_stats        ]" +
      "Row[        Key Conditions: (movie_genre = 'g1')]" +
      "Row[        Filter: (movie_name = 'm1')         ]");
    assertQuery("EXPLAIN DELETE FROM movie_stats WHERE movie_genre = 'g2';",
      "Row[Delete on imdb.movie_stats                  ]" +
      "Row[  ->  Range Scan on imdb.movie_stats        ]" +
      "Row[        Key Conditions: (movie_genre = 'g2')]");
  }

  @Test
  public void testUpdatePlan() throws Exception {
    assertQuery("EXPLAIN UPDATE movie_stats SET user_rank = 1 WHERE movie_name = 'm1'\n" +
        "and movie_genre = 'g1' and user_name = 'u1';",
      "Row[Update on imdb.movie_stats                               " +
        "                                  ]" +
      "Row[  ->  Primary Key Lookup on imdb.movie_stats             " +
        "                                  ]" +
      "Row[        Key Conditions: (movie_genre = 'g1') AND " +
        "(movie_name = 'm1') AND (user_name = 'u1')]");
  }

}
