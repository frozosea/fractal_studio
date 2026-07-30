"""Keep administrator and creator identities separate.

Revision ID: 20260730_0017
Revises: 20260730_0016
"""

from __future__ import annotations

from alembic import op


revision = "20260730_0017"
down_revision = "20260730_0016"
branch_labels = None
depends_on = None


def upgrade() -> None:
    op.execute(
        """
        CREATE FUNCTION enforce_admin_creator_role_separation()
        RETURNS trigger AS $$
        BEGIN
          PERFORM pg_advisory_xact_lock(hashtextextended(NEW.user_id::text, 0));

          IF NEW.role::text = 'admin' AND (
            EXISTS (
              SELECT 1 FROM user_roles
              WHERE user_id = NEW.user_id AND role::text = 'creator'
            ) OR EXISTS (
              SELECT 1 FROM creator_profiles WHERE user_id = NEW.user_id
            )
          ) THEN
            RAISE EXCEPTION 'administrator and creator identities must be separate'
              USING ERRCODE = '23514';
          END IF;

          IF NEW.role::text = 'creator' AND EXISTS (
            SELECT 1 FROM user_roles
            WHERE user_id = NEW.user_id AND role::text = 'admin'
          ) THEN
            RAISE EXCEPTION 'administrator and creator identities must be separate'
              USING ERRCODE = '23514';
          END IF;

          RETURN NEW;
        END;
        $$ LANGUAGE plpgsql
        """
    )
    op.execute(
        """
        CREATE TRIGGER user_roles_admin_creator_separation
        BEFORE INSERT OR UPDATE ON user_roles
        FOR EACH ROW EXECUTE FUNCTION enforce_admin_creator_role_separation()
        """
    )
    op.execute(
        """
        CREATE FUNCTION enforce_creator_profile_not_admin()
        RETURNS trigger AS $$
        BEGIN
          PERFORM pg_advisory_xact_lock(hashtextextended(NEW.user_id::text, 0));

          IF EXISTS (
            SELECT 1 FROM user_roles
            WHERE user_id = NEW.user_id AND role::text = 'admin'
          ) THEN
            RAISE EXCEPTION 'administrator accounts cannot have creator profiles'
              USING ERRCODE = '23514';
          END IF;
          RETURN NEW;
        END;
        $$ LANGUAGE plpgsql
        """
    )
    op.execute(
        """
        CREATE TRIGGER creator_profiles_admin_separation
        BEFORE INSERT OR UPDATE OF user_id ON creator_profiles
        FOR EACH ROW EXECUTE FUNCTION enforce_creator_profile_not_admin()
        """
    )


def downgrade() -> None:
    op.execute("DROP TRIGGER IF EXISTS creator_profiles_admin_separation ON creator_profiles")
    op.execute("DROP FUNCTION IF EXISTS enforce_creator_profile_not_admin()")
    op.execute("DROP TRIGGER IF EXISTS user_roles_admin_creator_separation ON user_roles")
    op.execute("DROP FUNCTION IF EXISTS enforce_admin_creator_role_separation()")
