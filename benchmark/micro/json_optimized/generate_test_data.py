#!/usr/bin/env python3
"""
Generate JSON test data for benchmarking
Creates datasets of various sizes with different schema patterns
"""

import json
import random
import sys
from pathlib import Path

def generate_consistent_record(record_id):
    """Generate a JSON record with consistent schema"""
    return {
        "user_id": record_id,
        "email": f"user{record_id}@example.com",
        "age": random.randint(18, 80),
        "status": random.choice(["active", "inactive", "pending"]),
        "score": round(random.uniform(0, 100), 2),
        "metadata": {
            "timestamp": f"2024-{random.randint(1,12):02d}-{random.randint(1,28):02d}T12:00:00Z",
            "source": random.choice(["web", "mobile", "api"]),
            "version": f"{random.randint(1,3)}.{random.randint(0,9)}.{random.randint(0,20)}"
        }
    }

def generate_inconsistent_record(record_id):
    """Generate a JSON record with varying schema"""
    base = {
        "id": record_id,
        "name": f"User {record_id}"
    }

    # Randomly include different fields
    if random.random() > 0.3:
        base["email"] = f"user{record_id}@example.com"
    if random.random() > 0.4:
        base["age"] = random.randint(18, 80)
    if random.random() > 0.5:
        base["status"] = random.choice(["active", "inactive"])
    if random.random() > 0.6:
        base["city"] = random.choice(["NYC", "SF", "LA", "CHI", "BOS"])
    if random.random() > 0.7:
        base["country"] = random.choice(["US", "UK", "CA", "AU"])
    if random.random() > 0.2:
        base["score"] = round(random.uniform(0, 100), 2)
    if random.random() > 0.5:
        base["tags"] = random.sample(["tech", "business", "sports", "music", "art"], k=random.randint(1, 3))
    if random.random() > 0.4:
        base["metadata"] = {
            "created": f"2024-{random.randint(1,12):02d}-{random.randint(1,28):02d}"
        }
        if random.random() > 0.5:
            base["metadata"]["updated"] = f"2024-{random.randint(1,12):02d}-{random.randint(1,28):02d}"

    return base

def generate_nested_record(record_id):
    """Generate deeply nested JSON record"""
    return {
        "id": record_id,
        "profile": {
            "name": f"User {record_id}",
            "email": f"user{record_id}@example.com",
            "contact": {
                "phone": f"+1-555-{random.randint(1000,9999)}",
                "address": {
                    "street": f"{random.randint(1,999)} Main St",
                    "city": random.choice(["NYC", "SF", "LA"]),
                    "zipcode": f"{random.randint(10000,99999)}"
                }
            }
        },
        "activity": {
            "logins": random.randint(1, 1000),
            "last_login": {
                "timestamp": f"2024-{random.randint(1,12):02d}-{random.randint(1,28):02d}T12:00:00Z",
                "ip": f"192.168.{random.randint(1,255)}.{random.randint(1,255)}"
            }
        }
    }

def write_jsonl(filename, records):
    """Write records as JSON Lines format"""
    with open(filename, 'w') as f:
        for record in records:
            f.write(json.dumps(record) + '\n')
    print(f"Generated {filename} with {len(records)} records ({Path(filename).stat().st_size / 1024 / 1024:.2f} MB)")

def main():
    output_dir = Path(__file__).parent / "data"
    output_dir.mkdir(exist_ok=True)

    sizes = {
        "1k": 1_000,
        "10k": 10_000,
        "100k": 100_000,
        "1m": 1_000_000,
    }

    # Only generate 10M if explicitly requested
    if len(sys.argv) > 1 and sys.argv[1] == "--large":
        sizes["10m"] = 10_000_000

    for size_name, count in sizes.items():
        print(f"\nGenerating {size_name} datasets...")

        # Consistent schema
        records = [generate_consistent_record(i) for i in range(count)]
        write_jsonl(output_dir / f"consistent_{size_name}.jsonl", records)

        # Inconsistent schema (more realistic)
        records = [generate_inconsistent_record(i) for i in range(count)]
        write_jsonl(output_dir / f"inconsistent_{size_name}.jsonl", records)

        # Nested schema
        records = [generate_nested_record(i) for i in range(count)]
        write_jsonl(output_dir / f"nested_{size_name}.jsonl", records)

if __name__ == "__main__":
    main()
