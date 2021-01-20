#!/usr/bin/env python
# -*- coding: utf-8 -*-
# Copyright (c) YugaByte, Inc.

import argparse
import json
import psycopg2
import sys


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--ssh_port', default=2345,
                        help='New custom ssh port')
    parser.add_argument('--db_user', default='postgres',
                        help='postgres DB username')
    args = parser.parse_args()
    con = None

    try:
        con = psycopg2.connect("host='localhost' dbname='yugaware' user='{}'".format(args.db_user))
        cur = con.cursor()
        cur.execute("SELECT node_uuid, node_details_json FROM node_instance")
        uuid_to_json = {}
        while True:
            row = cur.fetchone()
            if row is None:
                break
            uuid_to_json[str(row[0])] = str(row[1])

        for uuid, json_str in uuid_to_json.items():
            details_json = json.loads(json_str)
            details_json["sshPort"] = int(args.ssh_port)
            cur.execute("UPDATE node_instance SET node_details_json=%s WHERE node_uuid=%s",
                        (json.dumps(details_json), uuid))
            con.commit()

        print("Changed ssh port to {} for {} rows.".format(args.ssh_port, len(uuid_to_json)))

        # Final sanity checking.
        cur.execute("SELECT node_uuid, node_details_json FROM node_instance")
        while True:
            row = cur.fetchone()
            if row is None:
                break
            if str(args.ssh_port) not in row[1]:
                raise RuntimeError("Row with uuid='{}' not fixed, it is still '{}'."
                                   .format(row[0], row[1]))

    except psycopg2.DatabaseError as e:
        if con:
            con.rollback()

        print('Error %s' % e)
        sys.exit(1)

    finally:
        if con:
            con.close()


if __name__ == "__main__":
    main()
