"""Track the last creator display-name change for the 30-day cooldown."""

from alembic import op
import sqlalchemy as sa

revision = "20260825_0022"
down_revision = "20260810_0021"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.add_column(
        "creator_profiles",
        sa.Column("display_name_changed_at", sa.DateTime(timezone=True), nullable=True),
    )
    op.execute("UPDATE creator_profiles SET display_name_changed_at = CURRENT_TIMESTAMP")


def downgrade() -> None:
    op.drop_column("creator_profiles", "display_name_changed_at")
