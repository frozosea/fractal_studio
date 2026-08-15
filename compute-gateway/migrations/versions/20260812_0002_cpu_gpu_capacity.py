"""split node capacity into CPU and GPU resource pools

Revision ID: 20260812_0002
Revises: 20260802_0001
"""

import sqlalchemy as sa
from alembic import op

revision = "20260812_0002"
down_revision = "20260802_0001"
branch_labels = None
depends_on = None


def upgrade() -> None:
    for name, source in (
        ("max_cpu_slots", "max_durable_slots"),
        ("max_gpu_slots", "max_durable_slots"),
        ("max_cpu_preview_slots", "max_preview_slots"),
        ("max_gpu_preview_slots", "max_preview_slots"),
    ):
        op.add_column("compute_nodes", sa.Column(name, sa.Integer(), nullable=True))
        op.execute(sa.text(f"UPDATE compute_nodes SET {name} = {source}"))
        op.alter_column("compute_nodes", name, nullable=False)
        op.create_check_constraint(f"ck_compute_nodes_{name}", "compute_nodes", f"{name} BETWEEN 1 AND 64")


def downgrade() -> None:
    for name in ("max_gpu_preview_slots", "max_cpu_preview_slots", "max_gpu_slots", "max_cpu_slots"):
        op.drop_constraint(f"ck_compute_nodes_{name}", "compute_nodes", type_="check")
        op.drop_column("compute_nodes", name)
