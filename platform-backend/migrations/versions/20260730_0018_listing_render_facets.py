"""Denormalize render facets onto listings for marketplace browsing.

The values buyers care about — formula, iteration depth, output size and
colouring — live only inside two JSONB blobs today:
``fractal_recipes.canonical_spec`` and ``render_jobs.output_spec_json``. Reading
them per query means joining both tables and casting inside the predicate, which
no index can serve, and the colour facet additionally needs normalising because
``canonical_spec`` is serialised with ``exclude_none=True`` so ``colorMap`` and
``colorProgram`` are *absent* rather than null.

Resolving all of that once at publish time gives ordinary B-tree columns the
catalogue can filter and sort on, and captures the values immutably alongside
the rest of the published snapshot, so a listing keeps describing what was
actually bought even if the creator later edits the recipe.

Revision ID: 20260730_0018
Revises: 20260730_0017
"""

from __future__ import annotations

import sqlalchemy as sa
from alembic import op

revision = "20260730_0018"
down_revision = "20260730_0017"
branch_labels = None
depends_on = None


# Kept in step with `_FACET_DERIVATION` in app/marketplace/repository.py, which
# runs the same expressions when a draft is created. Alembic migrations stay
# self-contained, so the two are duplicated on purpose.
_FACET_EXPRESSIONS = """
    CASE WHEN jsonb_exists(r.canonical_spec, 'orbitProgram') THEN 'custom'
         ELSE r.canonical_spec ->> 'variant' END AS variant,
    -- transition_mesh carries its own iteration count, which overrides the recipe.
    COALESCE((j.output_spec_json ->> 'iterations')::int,
             (r.canonical_spec ->> 'iterations')::int) AS iterations,
    -- Images and videos carry width/height; meshes carry a single resolution.
    COALESCE((j.output_spec_json ->> 'width')::int,
             (j.output_spec_json ->> 'resolution')::int) AS output_width,
    COALESCE((j.output_spec_json ->> 'height')::int,
             (j.output_spec_json ->> 'resolution')::int) AS output_height,
    CASE WHEN jsonb_exists(r.canonical_spec, 'colorProgram') THEN 'custom_gradient'
         ELSE r.canonical_spec ->> 'colorMap' END AS color_map,
    r.canonical_spec ->> 'colorMode' AS color_mode,
    (r.canonical_spec ->> 'scale')::double precision AS view_scale
"""


def upgrade() -> None:
    op.add_column("listings", sa.Column("variant", sa.String(64)))
    op.add_column("listings", sa.Column("iterations", sa.Integer()))
    op.add_column("listings", sa.Column("output_width", sa.Integer()))
    op.add_column("listings", sa.Column("output_height", sa.Integer()))
    op.add_column("listings", sa.Column("color_map", sa.String(64)))
    op.add_column("listings", sa.Column("color_mode", sa.String(16)))
    op.add_column("listings", sa.Column("view_scale", sa.Float()))

    op.create_check_constraint(
        "ck_listings_iterations_positive",
        "listings",
        "iterations IS NULL OR iterations > 0",
    )
    op.create_check_constraint(
        "ck_listings_output_size_positive",
        "listings",
        "(output_width IS NULL OR output_width > 0) AND (output_height IS NULL OR output_height > 0)",
    )

    # Every listing already has a recipe and a render job: both FKs are NOT NULL
    # on `assets`, and `uq_assets_render_job` makes the job hop single-valued.
    op.execute(
        f"""
        UPDATE listings l
        SET variant = f.variant,
            iterations = f.iterations,
            output_width = f.output_width,
            output_height = f.output_height,
            color_map = f.color_map,
            color_mode = f.color_mode,
            view_scale = f.view_scale
        FROM (
            SELECT a.id AS asset_id, {_FACET_EXPRESSIONS}
            FROM assets a
            JOIN fractal_recipes r ON r.id = a.recipe_id
            JOIN render_jobs j ON j.id = a.render_job_id
        ) AS f
        WHERE f.asset_id = l.asset_id
        """
    )

    # Published snapshots are what a buyer's order points at, so they need the
    # same block or purchase history stays metadata-free for existing orders.
    op.execute(
        """
        UPDATE listing_versions v
        SET snapshot_json = v.snapshot_json || jsonb_build_object(
            'render', jsonb_strip_nulls(jsonb_build_object(
                'variant', l.variant,
                'iterations', l.iterations,
                'width', l.output_width,
                'height', l.output_height,
                'colorMap', l.color_map,
                'colorMode', l.color_mode,
                'viewScale', l.view_scale
            ))
        )
        FROM listings l
        WHERE l.id = v.listing_id AND NOT jsonb_exists(v.snapshot_json, 'render')
        """
    )

    # Partial composite indexes in the style of 20260724_0008: the catalogue only
    # ever reads published rows, and always orders by the published_at cursor.
    op.create_index(
        "ix_listings_facet_variant",
        "listings",
        ["status", "variant", "published_at", "id"],
        postgresql_where=sa.text("status = 'published'"),
    )
    op.create_index(
        "ix_listings_facet_color_map",
        "listings",
        ["status", "color_map", "published_at", "id"],
        postgresql_where=sa.text("status = 'published'"),
    )
    op.create_index(
        "ix_listings_facet_iterations",
        "listings",
        ["status", "iterations"],
        postgresql_where=sa.text("status = 'published'"),
    )

    # Marketplace search matches the creator's display name as well as the
    # handle, which already had a trigram index from 20260724_0008.
    op.execute(
        "CREATE INDEX ix_creator_profiles_display_name_trgm "
        "ON creator_profiles USING gin (display_name gin_trgm_ops)"
    )


def downgrade() -> None:
    op.execute("DROP INDEX IF EXISTS ix_creator_profiles_display_name_trgm")
    op.drop_index("ix_listings_facet_iterations", table_name="listings")
    op.drop_index("ix_listings_facet_color_map", table_name="listings")
    op.drop_index("ix_listings_facet_variant", table_name="listings")
    op.drop_constraint("ck_listings_output_size_positive", "listings", type_="check")
    op.drop_constraint("ck_listings_iterations_positive", "listings", type_="check")
    for column in ("view_scale", "color_mode", "color_map", "output_height", "output_width", "iterations", "variant"):
        op.drop_column("listings", column)
